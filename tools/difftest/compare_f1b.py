"""Comparator for the F1b differential harness: spec model vs reference interpreter vs generated
native code (production and reference builds), with interrupt-recognition policies.

Inputs: vectors.jsonl, the production lane file (N NL), the reference lane file (R RL I IF IR),
the spec model directory (the spec lane S is computed here, in-process, per vector: see
spec_runner_f1b.py), the generated sources (C++ excerpts, emitter feature counts). Output:
report.json and a summary on stdout; exit code 1 on any unexplained candidate, stale ledger entry,
host-fact violation or harness error.

Vectors are streamed: the three files are read in lockstep, one vector at a time, by --jobs worker
processes (vector index modulo jobs). Nothing but per-vector results is kept.

Native time (docs/native-time-20261005/DESIGN.md; the difftest_driver.cpp header): native code has no
instruction count and guest code runs in zero game time. Native lanes (N NL R RL) carry no "retired"
count; the interpreter lanes' "retired" (Cpu::interpreted_instructions) is compared only among the
interpreter lanes and with the spec. TB and DEC never move inside a vector, in any lane or in the
spec. RL re-runs every vector under the differential oracle (Cpu::set_verify_recompiled); its
VerifyReport ("verify") replaced the per-retire_block snapshots RL used to record. Lanes NB and N1
(budget-limited run(1) dispatch) are RETIRED: no instruction budget exists, so they would repeat N
and R exactly, and the driver refuses them.

Statuses. Every lane must end "ok", except on the event-spin vectors (event_spin(): gen_f1b.py's
tail family, shape event-spin/ram-poll, entered at the RAM-poll idle loop `lwz r4,0(r5) ; cmplwi
r4,0 ; beq self` on a zero word, no line asserted): such a loop can only be ended by an event and a
vector contains no event source, so native unlogged lanes end "watchdog" (the driver's
request_stop_from_host), native logged lanes "access-limit" (the driver's access cap; their logs
are dropped and not compared), interpreter lanes and the spec "step-limit", RL's oracle run
"watchdog", and every lane at the loop head with the loop's fixed-point state. Anywhere else those
statuses - and "irq-at-retired-count" (a line raised at a retired count: native time cannot express
it and gen_f1b.py no longer generates it), "host-stack-bound", "native-stop-not-at-stop-address",
"oracle-*" other than an oracle mismatch - are host facts. An oracle mismatch ("oracle-mismatch")
is RL's native code disagreeing with the interpreter: an engine divergence of RL (field "oracle").

Policy "project" (every vector without an asserted line):
  host facts    every lane ends with the status its vector allows at a stop address (event spins:
                at the loop head); MEM1 outside the data window unchanged; no data-access fault
                latched; native lanes interpreted nothing (g_non_native_ppc_executions and
                Cpu::interpreted_instructions 0), carry no retired count and kept the host stack
                within the driver's bound (host_stack_within_bound: the painted depth less the lane's
                calibrated depth <= 16 KiB); interpreter lanes stepped exactly their retired count;
                RL's oracle run ended where the logged run did (verify status, final_equal, mem_clean).
                RETIRED with the instruction clock: recompiled == retired (native code counts nothing;
                zero interpretation is what is left of it), the retire_block-to-retire_block host stack
                spread (HOST_STACK_BOUND here; the driver paints the stack and reports the verdict) and
                the RAM-poll coalescing counter (coalesce_ram_poll is deleted)
  engines       every engine lane equals I exactly: all GPR, both FPR halves, CR, XER, LR, CTR, MSR,
                FPSCR, pc, the 1024 SPRs, SR, TB, reservation, both lines, fault latch, data window,
                and the retired count between interpreter lanes; NL/RL access logs equal I's (event
                spins excepted: their logs end at the access cap by design)
  spec          S (substitution source I) equals I on the fields the spec State carries, masked only
                by residual undefined masks; state the spec does not carry is unchanged; S access log
                equals I's; no bounded-result violation

Policy "irq" (the external line asserted before entry, irq_ext 0: deliverable at entry, so taken at
the first recognition point - R1 at entry with EE = 1, R4/R5 where the guest's own mtmsr/rfi sets EE
- or latched and never delivered with EE = 0; lines raised at retired counts are retired):
  project lanes I IF R RL N NL must equal I exactly (engine divergence otherwise). The design claims
                native and interpreter take a condition at the same recognition point R1-R5.
  manual lane   IR (run_steps(1) without native code: R1 before every instruction) is the substitution
                source for S, and S must equal IR (a "manual-policy" candidate otherwise, including a
                recognition-point mismatch between IR and S)
  precision     every counted lane (I IF IR S) that stopped at the 0x500/0x900 vector is checked against
                the I trace at the boundary it recognised at (retired r): all architected state except
                SRR0, SRR1, MSR, pc and the lines must equal I's state after r instructions, SRR0 must be
                the pc there ("imprecise-state-at-entry" candidate otherwise), and SRR1/MSR/vector must
                equal the spec exception-entry model applied to that state ("entry-registers" candidate
                otherwise). A native lane has no count to locate its boundary with; its entry state is
                checked by its exact equality with I, which is itself checked here
  I vs S        a different recognition point is a "recognition-point difference": counted and
                listed, expected by the design, not a failure
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import multiprocessing
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compare as f1a            # noqa: E402  engine_fields, field_diffs, canonical_log, mem_bytes, excerpt, fmt
import emitter_classes as EC     # noqa: E402
import spec_runner_f1b as SR     # noqa: E402

ENGINE = ("I", "IF", "IR", "R", "RL", "N", "NL")
NATIVE = ("R", "RL", "N", "NL")
INTERP = ("I", "IF", "IR")
LOGGED = ("RL", "NL")
PROJECT = ("I", "IF", "R", "RL", "N", "NL")
M32 = 0xFFFFFFFF
SPEC_CARRIED_SPR = {26, 27, 19, 18, 1008, 920, 22} | set(range(272, 276)) | set(range(912, 920))
ENTRY_FIELDS = {"pc", "msr", "spr[26]", "spr[27]", "line_ext", "line_dec"}
# The event-spin vectors (gen_f1b.py fam_tail, EVENT_SPIN_SHAPE): the only vectors that may end at a bound.
EVENT_SPIN_SHAPE = "event-spin/ram-poll"
RAM_POLL_LOOP = (0x80850000, 0x28040000, 0x4182FFF8, 0x4E800020)   # lwz r4,0(r5) ; cmplwi r4,0 ; beq self ; blr
DATA_WINDOW = (0x80300000, 0x80308000)                             # zeroed by the driver before every vector

FPSCR_NAMES = ["FX", "FEX", "VX", "OX", "UX", "ZX", "XX", "VXSNAN", "VXISI", "VXIDI", "VXZDZ", "VXIMZ", "VXVC",
               "FR", "FI", "FPRF", "FPRF", "FPRF", "FPRF", "FPRF", "b20", "VXSOFT", "VXSQRT", "VXCVI", "VE", "OE",
               "UE", "ZE", "XE", "NI", "RN", "RN"]
MSR_NAMES = {13: "POW", 15: "ILE", 16: "EE", 17: "PR", 18: "FP", 19: "ME", 20: "FE0", 21: "SE", 22: "BE", 23: "FE1",
             25: "IP", 26: "IR", 27: "DR", 29: "PM", 30: "RI", 31: "LE"}


def bit_names(field, a, b):
    x = (a ^ b) & M32
    names = []
    for i in range(32):
        if not x & (1 << (31 - i)):
            continue
        if field == "fpscr":
            n = FPSCR_NAMES[i]
        elif field == "cr":
            n = f"CR{i // 4}"
        elif field in ("msr", "srr1", "spr[27]"):
            n = MSR_NAMES.get(i, f"b{i}")
            if field != "msr" and (1 <= i <= 4 or 10 <= i <= 15):
                n = f"cause{i}"
        elif field == "xer":
            n = {0: "SO", 1: "OV", 2: "CA"}.get(i, "count" if i >= 25 else f"b{i}")
        else:
            return field
        if n not in names:
            names.append(n)
    return f"{field}{{{','.join(names)}}}"


FPSCR_STATUS = 0xFFFFF700    # FX FEX VX OX UX ZX XX VXSNAN..VXVC FR FI FPRF VXSOFT VXSQRT VXCVI
FPSCR_CONTROL = 0x000008FF   # reserved bit 20, VE OE UE ZE XE NI RN


def fp_value_kind(a, b):
    """Coarse class of a 64-bit FPR image difference (candidate grouping only)."""
    def nan(x):
        return (x >> 52) & 0x7FF == 0x7FF and x & 0xFFFFFFFFFFFFF
    if nan(a) and nan(b):
        return "nan-bits"
    if nan(a) or nan(b):
        return "nan-vs-number"
    if (a & 0x7FFFFFFFFFFFFFFF) == 0 and (b & 0x7FFFFFFFFFFFFFFF) == 0:
        return "zero-sign"
    if (a >> 52) == (b >> 52) and abs((a & 0xFFFFFFFFFFFFF) - (b & 0xFFFFFFFFFFFFF)) <= (1 << 30):
        return "rounding"
    if a == 0 or b == 0 or (a & 0x7FFFFFFFFFFFFFFF) == 0 or (b & 0x7FFFFFFFFFFFFFFF) == 0:
        return "zero-vs-nonzero"
    return "value"


def norm_field(k, a=None, b=None):
    """Field name for candidate grouping: register indices dropped, bit-level registers by the category of
    the bits that differ (FPSCR status vs control bits, SRR1 exception-specific vs MSR-copy bits), FPR images
    by the kind of difference. The finding itself keeps the exact values."""
    ints = isinstance(a, int) and isinstance(b, int)
    if k == "fpscr" and ints:
        x = a ^ b
        cats = [n for n, m in (("status", FPSCR_STATUS), ("control", FPSCR_CONTROL)) if x & m]
        return "fpscr{" + ",".join(cats) + "}"
    if k in ("srr1", "spr[27]") and ints:
        x = a ^ b
        cats = [n for n, m in (("cause", 0x783F0000), ("msr-copy", ~0x783F0000 & M32)) if x & m]
        return k + "{" + ",".join(cats) + "}"
    if k in ("cr", "msr", "xer") and ints:
        return bit_names(k, a, b)
    if k.startswith("fpr[") and ints:
        return re.sub(r"\[\d+\]", "[]", k) + "{" + fp_value_kind(a, b) + "}"
    if k.startswith("spr["):
        return k
    return re.sub(r"\[\d+\]|\[0x[0-9A-Fa-f]+\]|@0x[0-9A-F]+", "[]", k)


def u(x):
    return int(x, 16) if isinstance(x, str) else int(x)


def spec_view(state):
    out = {f"gpr[{i}]": u(g) for i, g in enumerate(state["gpr"])}
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
    out["line_ext"] = bool(state["line_ext"])
    return out


def masked_diffs(a: dict, b: dict, masks: dict) -> list:
    out = []
    for key in sorted(set(a) | set(b)):
        va, vb = a.get(key, 0), b.get(key, 0)
        m = masks.get(key)
        if m is not None and not isinstance(va, bool) and isinstance(va, int) and isinstance(vb, int):
            if (va & ~m) == (vb & ~m):
                continue
        elif m is not None:
            continue
        if va != vb:
            out.append(key)
    return out


def untouched_diffs(initial, final):
    a, b = f1a.engine_fields(initial), f1a.engine_fields(final)
    out = []
    for k in sorted(set(a) | set(b)):
        carried = k.startswith(("gpr[", "fpr[")) or k in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "pc", "tb",
                                                          "retired", "reserve", "reserve_addr", "line_dec",
                                                          "line_ext")
        if k.startswith("spr[") and int(k[4:-1]) in SPEC_CARRIED_SPR:
            carried = True
        if not carried and a.get(k, 0) != b.get(k, 0):
            out.append(k)
    return out


def vector_offset(pc):
    if (pc & 0xFFF00000) == 0xFFF00000:
        return pc & 0xFFFFF
    if pc < 0x2000:
        return pc
    return None


def event_spin(v):
    """True only for gen_f1b.py's event-spin vectors, proven from the vector itself: family tail, shape
    EVENT_SPIN_SHAPE, no line asserted, entered at a row that is exactly the RAM-poll idle loop polling a
    word inside the data window that is zero (the driver zeroes the window and the vector's blobs leave the
    word 0; the loop stores nothing). Only an interrupt could end it, and a vector holds no event source."""
    if v.get("family") != "tail" or v.get("shape") != EVENT_SPIN_SHAPE or v.get("irq_ext") is not None:
        return False
    words = v["row_words"].get(f"{v['entry']:08X}")
    if words is None or tuple(words[:len(RAM_POLL_LOOP)]) != RAM_POLL_LOOP:
        return False
    ea = v["state"]["gpr"][5]
    if not (DATA_WINDOW[0] <= ea and ea + 4 <= DATA_WINDOW[1]):
        return False
    initial = {}
    for address, hexbytes in v["mem"]:
        for i, byte in enumerate(bytes.fromhex(hexbytes)):
            initial[address + i] = byte
    return all(initial.get(ea + i, 0) == 0 for i in range(4))


def expected_status(spin, lane):
    if not spin:
        return "ok"
    if lane in INTERP:
        return "step-limit"
    return "access-limit" if lane in LOGGED else "watchdog"


def engine_diff(ref, other):
    fi, fx = f1a.engine_fields(ref["final"]), f1a.engine_fields(other["final"])
    diffs = f1a.field_diffs(fi, fx)
    mi, mx = f1a.mem_bytes(ref["mem"]), f1a.mem_bytes(other["mem"])
    diffs += [f"mem[0x{a:08X}]" for a in sorted(set(mi) | set(mx)) if mi.get(a) != mx.get(a)]
    if ref["mem_clean"] != other["mem_clean"]:
        diffs.append("mem_clean")
    return diffs, fi, fx, mi, mx


def boundary_state(i_rec, r):
    """I's state after r retired instructions (r = 0: its initial state)."""
    if r == 0:
        return i_rec["initial"], []
    tr = i_rec.get("trace") or []
    if r - 1 < len(tr):
        return tr[r - 1]["s"], tr[r - 1]["mem"]
    return None, None


# ----------------------------------------------------------------------------------------------
def process_vector(v, rec, generated):
    vid = v["id"]
    policy = v.get("policy", "project")
    spin = event_spin(v)
    result = {"id": vid, "family": v["family"], "policy": policy, "spec": v["spec"], "reason": v["reason"],
              "note": v["note"], "divergent_lanes": [], "classes_spec": {}, "classes_engine": {},
              "recognition": None, "findings": [], "errors": [], "oracle": {}, "event_spin": spin, "info": []}
    missing = [ln for ln in ENGINE if ln not in rec]
    if missing:
        result["errors"].append(f"{vid}: missing lanes {missing}")
        return result
    source_lane = "IR" if policy == "irq" else "I"
    try:
        s = SR.run_vector(v, rec[source_lane])
    except Exception as e:                     # a spec-runner crash is a harness error, never a pass
        result["errors"].append(f"{vid}: spec runner raised {e!r}")
        return result
    rec["S"] = s
    # instruction classes the spec lane executed, in the emitter catalogue's naming (a faulting instruction
    # that retires nothing counts too)
    executed_words = [u(snap["word"]) for snap in s["trace"]] + \
        [u(ev["word"]) for ev in s["recognitions"] if ev.get("synchronous") and ev.get("word")]
    for w in executed_words:
        name = EC.emitter_class(w)
        result["classes_spec"][name] = result["classes_spec"].get(name, 0) + 1
    words = {}
    for a, ws in v["row_words"].items():
        for i, w in enumerate(ws):
            words[int(a, 16) + 4 * i] = w
    for snap in rec["I"].get("trace") or []:
        w = words.get(u(snap["pre"])) if "pre" in snap else None
        if w is not None:
            c = EC.emitter_class(w)
            result["classes_engine"][c] = result["classes_engine"].get(c, 0) + 1
    ver = rec["RL"].get("verify") or {}
    result["oracle"] = {k: int(ver.get(k, 0)) for k in ("verified", "verified_instructions", "skipped_device",
                                                        "skipped_exit", "skipped_hle", "mismatches")}

    # ---------------- host facts
    host = []
    stops = set(v.get("stops") or []) | {SR.SENTINEL}
    for ln in ENGINE:
        r = rec[ln]
        want = expected_status(spin, ln)
        # An oracle mismatch is not a host fact: RL's native code disagreed with the interpreter, an engine
        # divergence reported below (field "oracle").
        if r["status"] != want and not (ln == "RL" and r["status"] == "oracle-mismatch"):
            note = f1a.STATUS_NOTES.get(r["status"])
            host.append(f"{ln}: status {r['status']} ({r['host']['stop_detail'][:120]}), expected {want}" +
                        (f": {note}" if note else ""))
        elif spin:
            if u(r["final"]["pc"]) != v["entry"]:
                host.append(f"{ln}: event spin ended at 0x{u(r['final']['pc']):08X}, not at the loop head "
                            f"0x{v['entry']:08X}")
        elif u(r["final"]["pc"]) not in stops:
            host.append(f"{ln}: stopped at 0x{u(r['final']['pc']):08X}, not a stop address")
        if not r["mem_clean"]:
            host.append(f"{ln}: MEM1 outside the data window modified")
        if r["final"]["fault"]["latched"]:
            host.append(f"{ln}: data-access fault latched at EA {r['final']['fault']['effective']}")
    for ln in NATIVE:
        host += f1a.native_host_facts(ln, rec[ln])
    for ln in INTERP:
        if "retired" not in rec[ln]["final"]:
            host.append(f"{ln}: an interpreter lane without a retired count")
        elif rec[ln]["host"]["steps"] != rec[ln]["final"]["retired"]:
            host.append(f"{ln}: steps {rec[ln]['host']['steps']} != retired")
    host += f1a.oracle_host_facts(rec["RL"], "watchdog" if spin else "ok")
    if host:
        result["findings"].append({"id": vid, "family": v["family"], "class": "host-fact", "detail": host})

    # ---------------- engines
    divergent = {}
    compare_to_i = PROJECT[1:] if policy == "irq" else ENGINE[1:]
    for ln in compare_to_i:
        diffs, fi, fx, mi, mx = engine_diff(rec["I"], rec[ln])
        if diffs:
            divergent[ln] = {k: {"I": f1a.fmt(fi.get(k, 0)) if not k.startswith("mem[") else mi.get(int(k[4:-1], 16)),
                                 ln: f1a.fmt(fx.get(k, 0)) if not k.startswith("mem[") else mx.get(int(k[4:-1], 16))}
                             for k in diffs}
    oracle = f1a.oracle_divergence(rec["RL"])
    if oracle is not None:
        divergent.setdefault("RL", {})["oracle"] = oracle
    log_i = f1a.canonical_log(rec["I"]["log"])
    # An event spin's logged native lanes end at the driver's access cap, which drops their log by design.
    for ln in (() if spin else LOGGED):
        log_x = f1a.canonical_log(rec[ln]["log"])
        if log_x != log_i:
            first = next((i for i, (p, q) in enumerate(zip(log_i, log_x)) if p != q), min(len(log_i), len(log_x)))
            divergent.setdefault(ln, {})["access_log"] = {
                "first_index": first, "I": log_i[first] if first < len(log_i) else None,
                ln: log_x[first] if first < len(log_x) else None, "I_len": len(log_i), f"{ln}_len": len(log_x)}

    # ---------------- recognition (irq policy)
    rec_points = None
    if policy == "irq":
        rec_points = recognition(v, rec, s)
        result["recognition"] = rec_points
        # M1: every exception ends the vector at its architected vector, at either base (no vector code)
        stop_style = True
        lanes_r = rec_points["lanes"]

        def tab(ref_name, ref, ln, other):
            diffs, fi, fx, mi, mx = engine_diff(ref, other)
            return diffs, {k: {ref_name: f1a.fmt(fi.get(k, 0)) if not k.startswith("mem[") else mi.get(int(k[4:-1], 16)),
                               ln: f1a.fmt(fx.get(k, 0)) if not k.startswith("mem[") else mx.get(int(k[4:-1], 16))}
                           for k in diffs}

        def signature(r_):
            g = r_["final"]["gpr"]
            return (g[23], g[20], g[21])       # handler entries, last SRR0, last SRR1 (round trips)

        if stop_style:
            r_i, r_s, r_ir = (lanes_r.get(x, {}).get("r") for x in ("I", "S", "IR"))
            if r_i != r_s:
                result["info"].append({"kind": "recognition-point difference (project R1-R5 vs manual every boundary)",
                                       "r_I": r_i, "r_S": r_s, "k": rec_points["k"], "shape": v.get("shape")})
            if r_ir == r_i:
                diffs, table = tab("I", rec["I"], "IR", rec["IR"])
                if diffs:
                    divergent["IR"] = table
            if r_ir != r_s:
                result["findings"].append({
                    "id": vid, "family": v["family"], "note": v["note"], "class": "manual-recognition-mismatch",
                    "detail": {"r_IR": r_ir, "r_S": r_s, "k": rec_points["k"], "lanes": lanes_r}})
            for ln, info in lanes_r.items():
                if info.get("precise") is False:
                    result["findings"].append({
                        "id": vid, "family": v["family"], "note": v["note"], "class": "imprecise-state-at-entry",
                        "lane": ln, "detail": info, "first_divergence": {"mnemonic": info.get("mnemonic"),
                                                                         "pc": info.get("boundary_pc")}})
                if info.get("entry_registers_ok") is False:
                    result["findings"].append({
                        "id": vid, "family": v["family"], "note": v["note"], "class": "entry-registers",
                        "lane": ln, "detail": info, "first_divergence": {"mnemonic": info.get("exception")}})
        else:
            # round trips through the real 0x500 handler: the recognition point is guest-visible through the
            # handler's r23 (entries) / r20 (SRR0) / r21 (SRR1)
            sig_i, sig_ir = signature(rec["I"]), signature(rec["IR"])
            diffs, table = tab("I", rec["I"], "IR", rec["IR"])
            if diffs and sig_i != sig_ir:
                result["info"].append({"kind": "recognition-point difference (round trip, project vs manual)",
                                       "I": sig_i, "IR": sig_ir, "shape": v.get("shape")})
            elif diffs:
                divergent["IR"] = table
            rec_points["signatures"] = {ln: signature(rec[ln]) for ln in ENGINE}
            rec_points["signatures"]["S"] = (s["final"]["gpr"][23], s["final"]["gpr"][20], s["final"]["gpr"][21])

    # ---------------- spec
    spec_diffs = []
    spec_reason = None
    src = rec[source_lane]
    if spin and s["status"] != "step-limit":
        result["errors"].append(f"{vid}: event-spin vector, but the spec lane ended {s['status']} instead of "
                                f"reaching its step bound inside the loop")
    if s["status"] in ("spec-gap",):
        spec_reason = "spec-gap: " + "; ".join(s["notes"])[:300]
        if v["spec"] == "compare":
            result["errors"].append(f"{vid}: generator expected a spec comparison but {spec_reason}")
    elif s["status"] in ("undefined-all", "undefined-control"):
        spec_reason = s["status"] + ": " + "; ".join(s["notes"])[-300:]
    elif s["status"] == "step-limit" and not spin:
        result["errors"].append(f"{vid}: spec step limit")
    result["spec_reason"] = spec_reason
    if not spec_reason:
        sv = spec_view(s["final"])
        iv = SR.engine_view(src["final"])
        masks = {k: v_ for k, v_ in s["masks"].items() if not k.startswith("mem@")}
        spec_diffs = masked_diffs(sv, iv, masks)
        mem_masks = {int(k[4:], 16): m for k, m in s["masks"].items() if k.startswith("mem@")}
        ms, mi = f1a.mem_bytes(s["mem"]), f1a.mem_bytes(src["mem"])
        for addr in sorted(set(ms) | set(mi)):
            # Both tables list bytes that differ from the vector's initial window; a residual mask on
            # a whole byte (never substituted) is the only thing that excuses a difference.
            if ms.get(addr) != mi.get(addr) and mem_masks.get(addr, 0) != 0xFF:
                spec_diffs.append(f"mem[0x{addr:08X}]")
        if source_lane == "I":
            log_masks = substitution_mem_masks(s)
            if spec_log(s["log"], log_masks) != masked_log(log_i, log_masks):
                spec_diffs.append("access_log")
        spec_diffs += untouched_diffs(src["initial"], src["final"])
        for b in s["bounds"]:
            spec_diffs.append("bound")
        spec_diffs = sorted(set(spec_diffs))
    if s["internal"]:
        result["errors"].append(f"{vid}: spec-internal disagreement {json.dumps(s['internal'])[:400]}")

    result["divergent_lanes"] = sorted(divergent)
    # Facet split: FPSCR status/exception bits that the spec sets and the engines leave (plus the CR1 copy of
    # them an Rc=1 instruction makes) are one documented root cause shared by every FP instruction. They are
    # reported as their own common-mode finding so a value divergence in the same vector gets a clean signature.
    facet_fields = set()
    if spec_diffs and not divergent and "fpscr" in spec_diffs:
        sv0, iv0 = spec_view(s["final"]), SR.engine_view(src["final"])
        fx = (sv0["fpscr"] ^ iv0["fpscr"]) & ~s["masks"].get("fpscr", 0)
        cx = (sv0["cr"] ^ iv0["cr"]) & ~s["masks"].get("cr", 0)
        if fx and not fx & ~FPSCR_STATUS:
            facet = ["fpscr"] + (["cr"] if "cr" in spec_diffs and cx and not cx & ~0x0F000000 else [])
            result["findings"].append({
                "id": vid, "family": v["family"], "note": v["note"], "class": "common-mode", "facet": "fpscr-status",
                "spec_fields": {k: {"spec": f1a.fmt(sv0[k]), "interpreter": f1a.fmt(iv0[k]),
                                    "generated": f1a.fmt(SR.engine_view(rec["N"]["final"])[k]),
                                    "bits": bit_names(k, sv0[k], iv0[k])} for k in facet},
                "spec_first_divergence": locate_spec(v, rec, source_lane, only_fields=set(facet))})
            spec_diffs = [d for d in spec_diffs if d not in facet]
            facet_fields = set(facet)
    if not divergent and not spec_diffs:
        return result
    finding = {"id": vid, "family": v["family"], "note": v["note"], "entry": f"0x{v['entry']:08X}",
               "policy": policy, "rows": {k: [f"0x{w:08X}" for w in ws] for k, ws in v["row_words"].items()}}
    if divergent:
        finding["class"] = "engine-divergence"
        finding["lanes"] = divergent
        finding["first_divergence"] = locate_native(v, rec, divergent)
        if policy == "irq":
            # a recognition-point divergence is localised by the recognition points; any other divergence of an
            # irq vector (equal recognition points) keeps the run-level localisation
            if recognition_differs(rec_points, sorted(divergent)):
                finding["first_divergence"] = recognition_divergence(v, rec, rec_points, sorted(divergent)) or \
                    finding["first_divergence"]
            finding["recognition"] = rec_points
    if spec_diffs:
        sv = spec_view(s["final"])
        iv = SR.engine_view(src["final"])
        nv = SR.engine_view(rec["N"]["final"])      # no "retired": the generated column has none (native time)
        iv_i = SR.engine_view(rec["I"]["final"])
        ms, mi, mn = f1a.mem_bytes(s["mem"]), f1a.mem_bytes(src["mem"]), f1a.mem_bytes(rec["N"]["mem"])
        table = {}
        ini = f1a.engine_fields(src["initial"])
        fin_src = f1a.engine_fields(src["final"])
        fin_n = f1a.engine_fields(rec["N"]["final"])
        for k in spec_diffs:
            if k.startswith("mem["):
                a = int(k[4:-1], 16)
                table[k] = {"spec": ms.get(a), "interpreter": mi.get(a), "generated": mn.get(a)}
            elif k == "access_log":
                ls_ = spec_log(s["log"])
                first = next((i for i, (p, q) in enumerate(zip(ls_, log_i)) if p != q), min(len(ls_), len(log_i)))
                table[k] = {"first_index": first, "spec": ls_[first] if first < len(ls_) else None,
                            "interpreter": log_i[first] if first < len(log_i) else None,
                            "spec_len": len(ls_), "interpreter_len": len(log_i)}
            elif k == "bound":
                table[k] = {"spec": s["bounds"], "interpreter": None, "generated": None}
            elif k not in sv:
                table[k] = {"spec": f1a.fmt(ini.get(k, 0)), "interpreter": f1a.fmt(fin_src.get(k, 0)),
                            "generated": f1a.fmt(fin_n.get(k, 0)),
                            "note": "not carried by the spec State: a completed spec step leaves it unchanged"}
            else:
                table[k] = {"spec": f1a.fmt(sv.get(k)), "interpreter": f1a.fmt(iv.get(k)),
                            "generated": f1a.fmt(nv.get(k)),
                            "mask": f1a.fmt(s["masks"].get(k)) if k in s["masks"] else None}
                if source_lane != "I":
                    table[k]["interpreter_project_I"] = f1a.fmt(iv_i.get(k))
        cls = "common-mode" if not divergent else "engine-divergence"
        if policy == "irq":
            cls = "manual-policy" if not divergent else cls
        finding.setdefault("class", cls)
        finding["spec_fields"] = table
        finding["spec_status"] = s["status"]
        finding["spec_notes"] = s["notes"][:12]
        finding["substitutions"] = s["substitutions"][:12]
        finding["spec_first_divergence"] = locate_spec(v, rec, source_lane, exclude_fields=facet_fields)
    pc = (finding.get("spec_first_divergence") or {}).get("pc") or (finding.get("first_divergence") or {}).get("pc")
    if pc:
        finding["excerpt"] = f1a.excerpt(Path(generated), u(pc), _EXCERPT_CACHE)
    result["findings"].append(finding)
    return result


_EXCERPT_CACHE = {}


def substitution_mem_masks(s):
    """Byte address -> undefined-bit mask, for every data byte the spec left undefined (substituted or not).
    Only those bits of the access log's VALUES are excused; kinds, addresses and order are not."""
    out = {}
    for entry in s.get("substitutions") or []:
        for field, mask in entry["fields"].items():
            if field.startswith("mem@"):
                a = int(field[4:], 16)
                out[a] = out.get(a, 0) | int(mask, 16)
    for field, mask in (s.get("masks") or {}).items():
        if field.startswith("mem@"):
            a = int(field[4:], 16)
            out[a] = out.get(a, 0) | mask
    return out


def masked_log(seq, masks):
    return [(k, a, b & ~masks.get(a, 0)) for k, a, b in seq] if masks else seq


def spec_log(log, masks=None):
    seq = []
    for kind, addr, width, hexbytes in log:
        base = u(addr)
        data = bytes.fromhex(hexbytes)
        for i, byte in enumerate(data):
            seq.append((kind, base + i, byte))
    return masked_log(seq, masks or {})


# ----------------------------------------------------------------------------------------------
def recognition(v, rec, s):
    """Recognition points and precise-entry checks for an irq vector (the line asserted before entry)."""
    k = v.get("irq_ext")
    out = {"k": k, "source": "external", "lanes": {}}
    i_rec = rec["I"]
    for ln in ENGINE + ("S",):
        r = rec[ln] if ln != "S" else s
        fin = r["final"]
        pc = u(fin["pc"])
        off = vector_offset(pc)
        counted = "retired" in fin          # interpreter lanes and the spec; native lanes have no count
        info = {"final_pc": f"0x{pc:08X}"}
        if counted:
            info["retired"] = int(fin["retired"])
        if off in (0x500, 0x900):
            info["exception"] = "external" if off == 0x500 else "decrementer"
            if ln == "S":
                srr0 = u(fin["srr0"])
                lane_fields = None
            else:
                spr = {int(n): u(x) for n, x in fin["spr"]}
                srr0 = spr.get(26, 0)
                lane_fields = f1a.engine_fields(fin)
            info["srr0"] = f"0x{srr0:08X}"
            if not counted:
                # No instruction count locates this lane's recognition boundary in I's trace. Its entry state
                # (SRR0, SRR1, MSR, vector, every other register) is checked by its exact equality with I, whose
                # own entry is precision- and entry-register-checked below; a difference is an engine divergence.
                info["entry_checked_by"] = "exact equality with I (engine comparison)"
                out["lanes"][ln] = info
                continue
            rr = int(fin["retired"])
            info["r"] = rr
            ref_state, ref_mem = boundary_state(i_rec, rr)
            if ref_state is None:
                info["precise"] = None
                info["why"] = f"I trace has no boundary {rr}"
            else:
                ref_fields = f1a.engine_fields(ref_state)
                info["boundary_pc"] = f"0x{u(ref_state['pc']):08X}"
                if lane_fields is not None:
                    bad = [f for f in sorted(set(ref_fields) | set(lane_fields))
                           if f not in ENTRY_FIELDS and not f.startswith("fault") and
                           ref_fields.get(f, 0) != lane_fields.get(f, 0)]
                    mr, ml = f1a.mem_bytes(ref_mem or []), f1a.mem_bytes(r["mem"])
                    bad += [f"mem[0x{a:08X}]" for a in sorted(set(mr) | set(ml)) if mr.get(a) != ml.get(a)]
                else:
                    sv = spec_view(fin)
                    ev = SR.engine_view(ref_state)
                    bad = [f for f in sorted(sv) if f not in ("pc", "msr", "srr0", "srr1", "line_ext", "line_dec",
                                                              "exception") and f in ev and
                           (sv[f] & ~s["masks"].get(f, 0) if isinstance(sv[f], int) and not isinstance(sv[f], bool)
                            else sv[f]) != (ev[f] & ~s["masks"].get(f, 0) if isinstance(ev[f], int) and
                                            not isinstance(ev[f], bool) else ev[f])]
                if srr0 != u(ref_state["pc"]):
                    bad.append("srr0!=boundary_pc")
                info["precise"] = not bad
                if bad:
                    info["imprecise_fields"] = {f: {"boundary": f1a.fmt(ref_fields.get(f)) if not f.startswith("mem[")
                                                    and f != "srr0!=boundary_pc" else None,
                                                    "lane": f1a.fmt(lane_fields.get(f)) if lane_fields and
                                                    not f.startswith("mem[") and f != "srr0!=boundary_pc" else None}
                                                for f in bad[:20]}
                # SRR1 / MSR / vector against the spec entry model applied to the boundary state
                ok, detail = entry_model_check(v, ref_state, fin, ln == "S", info["exception"])
                info["entry_registers_ok"] = ok
                if not ok:
                    info["entry_registers"] = detail
                if 0 <= rr < len(s["trace"]) + 1 and rr - 1 < len(s["trace"]) and rr >= 1:
                    info["mnemonic"] = s["trace"][rr - 1]["mnemonic"]
        out["lanes"][ln] = info
    return out


def recognition_differs(rec_points, lanes):
    sig = rec_points.get("signatures")
    i_info = rec_points["lanes"].get("I", {})
    for ln in lanes:
        if sig is not None:
            if ln in sig and sig[ln] != sig["I"]:
                return True
            continue
        info = rec_points["lanes"].get(ln, {})
        if ln in NATIVE:
            # no count: the recognition point shows in where the lane stopped and what it saved in SRR0
            if (info.get("final_pc"), info.get("srr0")) != (i_info.get("final_pc"), i_info.get("srr0")):
                return True
        elif info.get("r") != i_info.get("r"):
            return True
    return False


def recognition_divergence(v, rec, rec_points, lanes):
    """First divergence of an irq vector whose project lanes disagree: the recognition points."""
    sig = rec_points.get("signatures")
    out = {"method": "recognition point", "divergent_lanes": lanes, "k": rec_points.get("k"),
           "source": rec_points.get("source")}
    per = {}
    for ln in ("I",) + tuple(lanes):
        info = rec_points["lanes"].get(ln, {})
        if sig is None:
            per[ln] = {"retired": info.get("r"), "srr0": info.get("srr0"), "final_pc": info.get("final_pc")}
        else:
            per[ln] = {"handler_entries": sig[ln][0], "srr0_r20": sig[ln][1], "srr1_r21": sig[ln][2]}
    out["lanes"] = per
    ln0 = lanes[0]
    srr0 = None
    if sig is None and rec_points["lanes"].get(ln0, {}).get("srr0"):
        srr0 = u(rec_points["lanes"][ln0]["srr0"])
    elif sig is not None:
        srr0 = u(sig[ln0][1])
    r0 = rec_points["lanes"].get(ln0, {}).get("r") if sig is None else None
    i_trace = rec["I"].get("trace") or []
    words = {int(a, 16) + 4 * i: w for a, ws in v["row_words"].items() for i, w in enumerate(ws)}
    if r0 is not None and 1 <= r0 <= len(i_trace) and "pre" in i_trace[r0 - 1]:
        pc_ = u(i_trace[r0 - 1]["pre"])
        w = words.get(pc_)
        out.update({"pc": f"0x{pc_:08X}", "word": f"0x{w:08X}" if w is not None else None,
                    "mnemonic": EC.emitter_class(w) if w is not None else None,
                    "note": f"lane {ln0} recognised the condition after this instruction (its retired count {r0}); "
                            f"lane I (same path) did not take it there"})
        return out
    if srr0 is not None:
        # the I trace follows the same path: the recognition point is after an instruction whose successor is
        # SRR0; a taken transfer (successor != pre + 4) is preferred, the first such occurrence wins
        hits = [snap for snap in i_trace if "pre" in snap and u(snap["s"]["pc"]) == srr0]
        taken = [snap for snap in hits if u(snap["s"]["pc"]) != (u(snap["pre"]) + 4) & 0xFFFFFFFF]
        pick = (taken or hits or [None])[0]
        if pick is not None:
            pc_ = u(pick["pre"])
            w = words.get(pc_)
            out.update({"pc": f"0x{pc_:08X}", "word": f"0x{w:08X}" if w is not None else None,
                        "mnemonic": EC.emitter_class(w) if w is not None else None,
                        "note": f"lane {ln0} recognised the condition before pc {srr0:#010x} (after this "
                                f"instruction, on the path lane I followed); lane I did not"})
    return out if "pc" in out else None


def entry_model_check(v, ref_state, fin, is_spec, taken):
    mods = SR._MODS
    gs, exc = mods["state"], mods["exc"]
    st = gs.State()
    ev = SR.engine_view(ref_state)
    st.msr = ev["msr"]
    st.pc = ev["pc"]
    st.srr0, st.srr1 = ev["srr0"], ev["srr1"]
    st.pending = {"external": taken == "external", "decrementer": taken == "decrementer"}
    try:
        exc.take_pending(st)
        masks = {}
    except exc.UndefinedBehaviour as e:
        kind, masks = SR.norm_undefined(exc, e)
        if kind == "all":
            return True, {"note": "entry boundedly undefined"}
        e.apply_defined(st)
    if is_spec:
        lane = {"srr0": u(fin["srr0"]), "srr1": u(fin["srr1"]), "msr": u(fin["msr"]), "pc": u(fin["pc"])}
    else:
        spr = {int(n): u(x) for n, x in fin["spr"]}
        lane = {"srr0": spr.get(26, 0), "srr1": spr.get(27, 0), "msr": u(fin["msr"]), "pc": u(fin["pc"])}
    exp = {"srr0": st.srr0, "srr1": st.srr1, "msr": st.msr, "pc": st.pc}
    bad = {}
    for f in ("srr0", "srr1", "msr", "pc"):
        m = masks.get(f, 0)
        if (lane[f] & ~m & M32) != (exp[f] & ~m & M32):
            bad[f] = {"spec_entry_model": f"0x{exp[f]:08X}", "lane": f"0x{lane[f]:08X}", "mask": f"0x{m:08X}",
                      "bits": bit_names("srr1" if f == "srr1" else ("msr" if f == "msr" else f), lane[f] & ~m,
                                        exp[f] & ~m) if f in ("srr1", "msr") else f}
    return not bad, bad


# ----------------------------------------------------------------------------------------------
def locate_spec(v, rec, source_lane, only_fields=None, exclude_fields=None):
    s_trace = rec["S"]["trace"]
    i_trace = rec[source_lane].get("trace") or []
    masks = rec["S"]["masks"]
    src = rec[source_lane]
    events = defaultdict(list)
    for r_ in rec["S"].get("recognitions") or []:
        events[r_["boundary"]].append(r_)
    for k, snap in enumerate(s_trace):
        pre = i_trace[k].get("pre") if k < len(i_trace) else None
        for ev in events.get(k, []):
            # the spec took an exception at this boundary before retiring step k; if the source lane's k-th
            # retired instruction is not the spec's, that exception is where the lanes part
            if ev.get("synchronous") and not ev.get("word"):
                continue
            if pre is None or u(pre) != u(snap["from_pc"]) or not ev.get("synchronous"):
                return event_divergence(ev, k, rec, source_lane)
        if k >= len(i_trace):
            return {"step": k, "pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                    "reason": f"{source_lane} trace ended"}
        sv = spec_view(snap)
        iv = SR.engine_view(i_trace[k]["s"])
        diffs = masked_diffs(sv, iv, {kk: m for kk, m in masks.items() if not kk.startswith("mem@")})
        diffs = [d for d in diffs if d not in ("exception",)]
        ms, mi = f1a.mem_bytes(snap["mem"]), f1a.mem_bytes(i_trace[k]["mem"])
        diffs += [f"mem[0x{x:08X}]" for x in sorted(set(ms) | set(mi)) if ms.get(x) != mi.get(x)]
        untouched = untouched_diffs(src["initial"], i_trace[k]["s"])
        if only_fields is not None:
            diffs = [d for d in diffs if d in only_fields]
            untouched = []
        if exclude_fields:
            diffs = [d for d in diffs if d not in exclude_fields]
        if diffs or untouched:
            fields = {}
            for d in diffs:
                if d.startswith("mem["):
                    a = int(d[4:-1], 16)
                    fields[d] = {"spec": ms.get(a), "interpreter": mi.get(a)}
                else:
                    fields[d] = {"spec": f1a.fmt(sv.get(d)), "interpreter": f1a.fmt(iv.get(d))}
            ini, now = f1a.engine_fields(src["initial"]), f1a.engine_fields(i_trace[k]["s"])
            for d in untouched:
                fields[d] = {"spec": f1a.fmt(ini.get(d, 0)), "interpreter": f1a.fmt(now.get(d, 0)),
                             "note": "state the spec State does not carry; unchanged by a completed spec step"}
            before = s_trace[k - 1] if k > 0 else None
            fpscr_b = u(before["fpscr"]) if before else rec["S"]["initial_fpscr"]
            msr_b = u(before["msr"]) if before else rec["S"]["initial_msr"]
            return {"step": k, "pc": snap["from_pc"], "word": snap["word"], "mnemonic": snap["mnemonic"],
                    "fields": fields, "generated_snapshot": f1a.NO_NATIVE_SNAPSHOT,
                    "fpscr_before": f"0x{fpscr_b:08X}", "msr_before": f"0x{msr_b:08X}"}
    # traces agree up to the spec's last retired instruction: a non-retiring event after it
    last = s_trace[-1] if s_trace else None
    tail = events.get(len(s_trace), [])
    if tail:
        return event_divergence(tail[-1], len(s_trace), rec, source_lane)
    if len(i_trace) > len(s_trace):
        return {"step": len(s_trace), "reason": "spec trace ended (status %s)" % rec["S"]["status"],
                "pc": s_trace[-1]["from_pc"] if s_trace else f"0x{v['entry']:08X}",
                "mnemonic": s_trace[-1]["mnemonic"] if s_trace else None}
    return {"reason": "per-step traces agree; divergence only in the final comparison",
            "pc": last["from_pc"] if last else f"0x{v['entry']:08X}",
            "mnemonic": last["mnemonic"] if last else None}


def event_divergence(ev, k, rec, source_lane):
    """An exception the spec took at boundary k (synchronous fault or asynchronous condition)."""
    src = rec[source_lane]
    if ev.get("synchronous"):
        word = u(ev["word"])
        mods = SR.owners(word)
        name = mods[0].mnemonic(word) if mods else None
        return {"step": k, "pc": ev["from_pc"], "word": ev["word"], "mnemonic": name, "exception": ev["exception"],
                "vector": ev["vector"], "reason": f"the spec takes a {ev['exception']} exception at this instruction; "
                f"the {source_lane} lane does not (it retires it or continues elsewhere)",
                "engine_after": {"pc": src["final"]["pc"], "retired": src["final"]["retired"]}}
    return {"step": k, "pc": ev["srr0"], "mnemonic": ev["exception"], "exception": ev["exception"],
            "vector": ev["vector"], "reason": f"the spec recognises the {ev['exception']} condition at boundary {k}"}


def locate_native(v, rec, divergent):
    """Native time: native code publishes no per-instruction or per-block state (the RL retire_block snapshots,
    aligned by retired count, went with the instruction clock), so the window is the whole interpreter run.
    The RL oracle's first mismatching segment is reported when it saw one; main() then tries the prefix
    vectors; inside the run, the first instruction whose interpreter step writes a field that diverges in the
    final states names the candidate instruction."""
    i_trace = rec["I"].get("trace") or []
    s_trace = list(rec["S"]["trace"])
    divergent_lanes = sorted(divergent)
    result = {"divergent_lanes": divergent_lanes}
    segment = f1a.oracle_first_segment(rec)
    if segment:
        result["oracle_segment"] = segment
    if not s_trace:
        # No spec trace (an engine-only vector): name instructions from the interpreter's own trace, the
        # address it executed and the row word there, in the emitter catalogue's naming.
        words = {int(a, 16) + 4 * i: w for a, ws in v["row_words"].items() for i, w in enumerate(ws)}
        for snap in i_trace:
            if "pre" not in snap or u(snap["pre"]) not in words:
                break
            w = u(words[u(snap["pre"])])
            s_trace.append({"from_pc": f"0x{u(snap['pre']):08X}", "word": f"0x{w:08X}",
                            "mnemonic": EC.emitter_class(w), "variant": None})
        if s_trace:
            result["names_from"] = "interpreter trace (no spec trace for this vector)"
    window = (0, int(rec["I"]["final"]["retired"]))
    result["window_retired"] = list(window)
    lo, hi = window
    result["window_instructions"] = [f"{x['from_pc']} {x['word']} {x['variant'] or x['mnemonic']}"
                                     for x in s_trace[lo:hi]][:64]
    if hi - lo == 1 and hi - 1 < len(s_trace):
        x = s_trace[hi - 1]
        result.update({"pc": x["from_pc"], "word": x["word"], "mnemonic": x["mnemonic"],
                       "method": "a run of one instruction"})
        return result
    # pc changes at every step and names no instruction; the other non-state keys are not written by a step
    divergentf = {k for table in divergent.values() for k in table
                  if k not in ("pc", "retired", "access_log", "oracle", "mem_clean")}
    if divergentf and s_trace:
        for kk in range(lo, min(hi, len(i_trace))):
            after = f1a.engine_fields(i_trace[kk]["s"])
            before = f1a.engine_fields(i_trace[kk - 1]["s"]) if kk > 0 else f1a.engine_fields(rec["I"]["initial"])
            mb = f1a.mem_bytes(i_trace[kk - 1]["mem"]) if kk > 0 else {}
            ma = f1a.mem_bytes(i_trace[kk]["mem"])
            written = {f for f in divergentf if not f.startswith("mem[") and before.get(f) != after.get(f)}
            written |= {f for f in divergentf if f.startswith("mem[") and mb.get(int(f[4:-1], 16)) != ma.get(int(f[4:-1], 16))}
            if written and kk < len(s_trace):
                x = s_trace[kk]
                result.update({"pc": x["from_pc"], "word": x["word"], "mnemonic": x["mnemonic"],
                               "method": f"first instruction of the run {window} whose interpreter step "
                                         f"writes a divergent field ({sorted(written)[:6]})"})
                return result
    if s_trace and lo < len(s_trace):
        x = s_trace[lo]
        result.update({"pc": x["from_pc"], "word": x["word"], "mnemonic": x["mnemonic"],
                       "method": f"first instruction of the run {window}"})
    elif s_trace:
        x = s_trace[-1]
        result.update({"pc": x["from_pc"], "word": x["word"], "mnemonic": x["mnemonic"],
                       "method": "after the last retired instruction (exception entry or stop)"})
    return result


# ----------------------------------------------------------------------------------------------
_ID = re.compile(r'"id":\s*"([^"]+)"')


class LaneReader:
    def __init__(self, path):
        self.f = open(path, encoding="utf-8")
        self.pending = None
        self.summaries = []

    def group(self, vid):
        """All raw lines of vector vid (the file is in vector order), or None if absent."""
        lines = []
        while True:
            line = self.pending if self.pending is not None else self.f.readline()
            self.pending = None
            if not line:
                return lines or None
            if line.startswith('{"lane_summary"'):
                self.summaries.append(json.loads(line))
                continue
            m = _ID.search(line[:200])
            lid = m.group(1) if m else None
            if lid != vid:
                self.pending = line
                return lines or None
            lines.append(line)


def worker(args):
    index, jobs, paths = args
    SR.load(Path(paths["spec"]))
    prod, ref = LaneReader(paths["prod"]), LaneReader(paths["ref"])
    only = set(paths["only"]) if paths["only"] else None
    results = []
    summaries = []
    with open(paths["vectors"], encoding="utf-8") as vf:
        for n, vline in enumerate(vf):
            m = _ID.search(vline[:400])
            vid = m.group(1) if m else None
            gp, gr = prod.group(vid), ref.group(vid)
            if only is not None and vid not in only:
                continue
            if n % jobs != index:
                continue
            v = json.loads(vline)
            rec = {}
            for line in (gp or []) + (gr or []):
                r = json.loads(line)
                rec[r["lane"]] = r
            results.append((n, process_vector(v, rec, paths["generated"])))
    # drain summaries
    for reader in (prod, ref):
        while reader.group("\x00") is not None:
            pass
        summaries += reader.summaries
    return results, summaries


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--vectors", type=Path, required=True)
    p.add_argument("--prod", type=Path, required=True)
    p.add_argument("--ref", type=Path, required=True)
    p.add_argument("--spec", type=Path, required=True)
    p.add_argument("--generated", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--known", type=Path, default=None)
    p.add_argument("--jobs", type=int, default=8)
    p.add_argument("--only", default="")
    p.add_argument("--max-detail", type=int, default=600)
    a = p.parse_args()

    paths = {"vectors": str(a.vectors), "prod": str(a.prod), "ref": str(a.ref), "spec": str(a.spec),
             "generated": str(a.generated), "only": [x for x in a.only.split(",") if x]}
    jobs = max(1, a.jobs)
    if jobs == 1:
        outs = [worker((0, 1, paths))]
    else:
        with multiprocessing.Pool(jobs) as pool:
            outs = pool.map(worker, [(i, jobs, paths) for i in range(jobs)])
    results = sorted((r for o in outs for r in o[0]), key=lambda t: t[0])
    summaries = outs[0][1]

    vectors_meta = {}
    counts = Counter()
    errors = []
    findings = []
    engine_only = Counter()
    classes_spec = Counter()
    classes_engine = Counter()
    info = Counter()
    info_examples = defaultdict(list)
    recog = []
    divergent_by_id = {}
    oracle_totals = Counter()
    event_spins = []
    for s_ in summaries:
        if not s_["locked_cache_clean"]:
            errors.append(f"lane {s_['lane_summary']}: locked cache modified")
    for _, r in results:
        counts["vectors"] += 1
        vectors_meta[r["id"]] = r
        divergent_by_id[r["id"]] = set(r["divergent_lanes"])
        errors += r["errors"]
        if r.get("spec_reason"):
            counts["engine-only"] += 1
            engine_only[r["spec_reason"].split(":")[0] + ": " + (r["reason"] or r["spec_reason"][:100])] += 1
        else:
            counts["spec-compared"] += 1
            classes_spec.update(r["classes_spec"])
        classes_engine.update(r["classes_engine"])
        for i in r["info"]:
            info[i["kind"]] += 1
            if len(info_examples[i["kind"]]) < 40:
                info_examples[i["kind"]].append(dict(i, id=r["id"], note=r["note"]))
        if r["recognition"] is not None:
            recog.append({"id": r["id"], "family": r["family"], "note": r["note"], **r["recognition"]})
        oracle_totals.update(r.get("oracle") or {})
        if r.get("oracle") is not None and not (r.get("oracle") or {}).get("verified"):
            oracle_totals["vectors_with_no_verified_segment"] += 1
        if r.get("event_spin"):
            event_spins.append(r["id"])
        if not r["findings"]:
            counts["pass"] += 1
        for f in r["findings"]:
            counts[f["class"]] += 1
            findings.append(f)

    # prefix localisation (needs the children's divergence)
    children = defaultdict(list)
    vec_by_id = {}
    with a.vectors.open(encoding="utf-8") as vf:
        for line in vf:
            v = json.loads(line)
            if v.get("prefix_k") is not None:
                children[v["parent"]].append((v["prefix_k"], v["id"], v))
            if v["id"] in divergent_by_id:
                vec_by_id[v["id"]] = {"entry": v["entry"], "row_words": v["row_words"], "mnemonic": v.get("mnemonic")}
    for f in findings:
        if f["class"] != "engine-divergence":
            continue
        fd = f.get("first_divergence") or {}
        kids = sorted(children.get(f["id"], []))
        if not kids or fd.get("method", "").startswith("a run of one"):
            continue
        lanes = set(fd.get("divergent_lanes") or [])
        for kpos, kid, kv in kids:
            if divergent_by_id.get(kid, set()) & lanes:
                parent = vec_by_id[f["id"]]
                row = parent["row_words"][f"{parent['entry']:08X}"]
                fd.update({"method": f"smallest diverging prefix {kid} (words[0..{kpos}) + blr)",
                           "pc": f"0x{parent['entry'] + 4 * (kpos - 1):08X}", "word": f"0x{row[kpos - 1]:08X}",
                           "mnemonic": (kv.get("mnemonic") or "").split(",")[-1] or None})
                break

    # candidate grouping
    groups = defaultdict(list)
    for f in findings:
        if f["class"] == "host-fact":
            key = ("host-fact", tuple(sorted(set(d.split(":")[0] + ":" + d.split(":")[1].split("(")[0][:60]
                                                 for d in f["detail"]))))
        elif f["class"] in ("imprecise-state-at-entry", "entry-registers", "manual-recognition-mismatch"):
            det = f.get("detail") or {}
            fields = sorted(set(norm_field(x) for x in (det.get("imprecise_fields") or {})) |
                            set(x for x in (det.get("entry_registers") or {}))) if isinstance(det, dict) else []
            key = (f["class"], f.get("lane"), det.get("exception") if isinstance(det, dict) else None,
                   tuple(fields), f["family"])
        else:
            fd = f.get("spec_first_divergence") if f["class"] in ("common-mode", "manual-policy") else \
                f.get("first_divergence")
            mnemonic = (fd or {}).get("mnemonic")
            names = set()
            for k, row in (f.get("spec_fields") or {}).items():
                names.add(norm_field(k, u(row["spec"]) if isinstance(row.get("spec"), str) else None,
                                     u(row["interpreter"]) if isinstance(row.get("interpreter"), str) else None))
            for ln, tab in (f.get("lanes") or {}).items():
                for k, row in tab.items():
                    vals = list(row.values()) if isinstance(row, dict) else []
                    a_ = u(vals[0]) if len(vals) == 2 and isinstance(vals[0], str) else None
                    b_ = u(vals[1]) if len(vals) == 2 and isinstance(vals[1], str) else None
                    names.add(norm_field(k, a_, b_))
            key = (f["class"], mnemonic, tuple(sorted((f.get("lanes") or {}).keys())), tuple(sorted(names)))
        groups[key].append(f)
    candidates = []
    for key, members in sorted(groups.items(), key=lambda kv: (-len(kv[1]), str(kv[0]))):
        fp = hashlib.sha1(json.dumps(sorted(
            json.dumps({k: m.get(k) for k in ("id", "class", "lanes", "spec_fields", "detail", "lane")},
                       sort_keys=True, default=str) for m in members)).encode()).hexdigest()
        candidates.append({"signature": [list(x) if isinstance(x, tuple) else x for x in key], "count": len(members),
                           "vectors": [m["id"] for m in members], "fingerprint": fp, "example": members[0]})
    known = json.loads(a.known.read_text(encoding="utf-8")) if a.known and a.known.exists() else []
    known_fps = {k["fingerprint"]: k for k in known}
    # host facts are never satisfiable by the ledger, whatever it contains
    unexplained = [c for c in candidates if c["fingerprint"] not in known_fps or c["signature"][0] == "host-fact"]
    stale = [k for k in known if k["fingerprint"] not in {c["fingerprint"] for c in candidates}]

    # emitter coverage
    features = Counter()
    patterns = dict(f1a_patterns())
    for shard in sorted(a.generated.glob("recomp_[0-9][0-9][0-9].cpp")):
        text = shard.read_text(encoding="utf-8")
        for name, pattern in patterns.items():
            features[name] += len(re.findall(pattern, text, re.M))
    rep = (a.generated / "recomp_report.txt").read_text(encoding="utf-8") \
        if (a.generated / "recomp_report.txt").exists() else ""
    for label in ("fp checks emitted", "fp checks hoisted"):
        mm = re.search(label + r"\s*:\s*(\d+)", rep)
        if mm:
            features[label.replace(" ", "_")] = int(mm.group(1))
    all_classes = EC.all_classes()
    executed = set(classes_engine)
    class_table = {c: {"engine_executions": classes_engine.get(c, 0)} for c in all_classes}
    never = [c for c in all_classes if c not in executed]
    lat = Counter()
    for r_ in recog:
        ri = r_["lanes"].get("I", {}).get("r")
        rs = r_["lanes"].get("S", {}).get("r")
        lat["taken-by-neither" if ri is None and rs is None else
            ("project-not-taken" if ri is None else ("manual-not-taken" if rs is None else
                                                    f"latency {ri - rs:+d}"))] += 1
    report = {
        "summary": dict(counts),
        "harness_errors": errors[:400],
        "harness_errors_total": len(errors),
        "engine_only": dict(engine_only),
        "candidates": [{k: c[k] for k in ("signature", "count", "vectors", "fingerprint")} for c in candidates],
        "unexplained_candidates": [c["fingerprint"] for c in unexplained],
        "stale_known": stale,
        "findings": findings[:a.max_detail],
        "findings_total": len(findings),
        "candidate_examples": [{"fingerprint": c["fingerprint"], "example": c["example"]} for c in candidates],
        "info": dict(info),
        "info_examples": info_examples,
        "recognition_latency_histogram": dict(lat),
        "recognition": recog[:3000],
        "spec_compared_instruction_classes": dict(classes_spec),
        "emitter_classes": class_table,
        "emitter_classes_never_executed": never,
        "emitter_features_in_generated_code": dict(features),
        "rl_oracle": dict(oracle_totals),
        "event_spin_vectors": event_spins,
        "lane_summaries": summaries,
    }
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=1, default=str), encoding="utf-8")

    print(f"difftest F1b compare: {counts['vectors']} vectors, {counts['pass']} pass, {counts['spec-compared']} "
          f"spec-compared, {counts['engine-only']} engine-only, {counts['engine-divergence']} engine-divergence, "
          f"{counts['common-mode']} common-mode, {counts['manual-policy']} manual-policy, "
          f"{counts['imprecise-state-at-entry']} imprecise-entry, {counts['entry-registers']} entry-registers, "
          f"{counts['manual-recognition-mismatch']} manual-recognition-mismatch, {counts['host-fact']} host-fact, "
          f"{len(errors)} harness errors")
    print(f"  RL differential oracle: {oracle_totals['verified']} segments / {oracle_totals['verified_instructions']} "
          f"instructions verified, {oracle_totals['mismatches']} mismatches, skipped {oracle_totals['skipped_exit']} "
          f"(no recognisable end) {oracle_totals['skipped_device']} (device) {oracle_totals['skipped_hle']} (HLE); "
          f"{oracle_totals['vectors_with_no_verified_segment']} vectors with no verified segment")
    print(f"  event-spin vectors (bounded by the harness, not by a stop address): {len(event_spins)} {event_spins[:8]}")
    print(f"  emitter classes: {len(all_classes)} catalogued, {len(all_classes) - len(never)} executed, "
          f"never executed: {never}")
    for kind, n in info.items():
        print(f"  info {n:5d}  {kind}")
    print(f"  recognition latency (I vs S): {dict(lat)}")
    for reason, n in sorted(engine_only.items(), key=lambda kv: -kv[1])[:40]:
        print(f"  engine-only {n:4d}  {reason[:160]}")
    for c in candidates:
        tag = "KNOWN " + known_fps[c["fingerprint"]]["candidate"] if c["fingerprint"] in known_fps else "NEW"
        ex = c["example"]
        print(f"  [{tag}] {c['count']:4d} x {json.dumps(c['signature'])[:260]}  e.g. {ex['id']} {ex.get('note', '')[:60]}")
    for e in errors[:30]:
        print("  HARNESS ERROR " + e[:300])
    for k in stale:
        print(f"  STALE known candidate {k['candidate']} ({k['fingerprint']}) no longer reproduces")
    print(f"report: {a.out}")
    return 1 if (errors or unexplained or stale) else 0


def f1a_patterns():
    base = {
        "cr_fusion": r"cnd = !?\((static_cast<s32>\(c\.gpr|c\.gpr\[\d+\] [<>=])",
        "unfused_cr_bit": r"ppc::cr_bit\(c, ",
        "ctr_decrement": r"--c\.ctr;",
        # Native time: a cold (mid-record) entry is a slot switch; the kEntrySkip/unretire retirement literal it
        # carried, the conditional fall-through retire_block batch, the RAM-poll coalescing call
        # (coalesce_ram_poll) and the retire_block flush before mfspr/mtspr of TB/DEC (TB and DEC are views of
        # the game clock) went with the instruction clock.
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
        "mem_read_write": r"c\.mem_(read|write)(8|16|32|64)\(",
        "lwarx_stwcx": r"ppc::(lwarx|stwcx)\(",
        "dcbz_loop": r"for \(u32 i = 0; i < 32u; i \+= 4u\)",
        "end_of_function_leave": r"FallbackReason::EndOfFunction",
        "fp_unavailable_guard": r"recomp::fp_unavailable\(c, ",
        "gekko_extension_guard": r"recomp::gekko_extension_fault\(c, ",
        "hid2_illegal_guard": r"recomp::illegal_instruction\(c, ",
        "mtmsr_recognition_checks": r"if \(c\.interrupt_pending\(\)\) \{ c\.leave_to_dispatcher",
        "system_call": r"recomp::system_call\(c, ",
        "return_from_interrupt": r"recomp::return_from_interrupt\(c, ",
        "trap_lowering": r"if \(ppc::trap_taken\(",
        "psq_load_store": r"ppc::psq_(load|store)\(c, ",
        "fp_double_helpers": r"ppc::fp_(add|sub|mul|div|sqrt|rsqrte|madd|msub|nmadd|nmsub)\(c, ",
        "fp_single_helpers": r"ppc::fps_(add|sub|mul|div|sqrt|res|rsqrte|round|madd|msub|nmadd|nmsub)\(c, ",
        "fp_compare": r"ppc::fp_compare\(c, ",
        "fctiw": r"ppc::fctiw\(c, ",
        "update_cr1": r"ppc::update_cr1\(c\)",
        "validate_native_code": r"c\.validate_native_code\(",
    }
    return base


if __name__ == "__main__":
    sys.exit(main())
