"""The player coverage audit (docs/SHADOW_PLAYER_COVERAGE_AUDIT.md): the census membership re-graded from the binary,
the instruction inventory recomputed from main.dol, verification coverage by run, the fast/strict gap explained per
instruction, and one status per function.

    python -B -m dolrecon.player_audit <dolrecon root> <player_census.csv> <out dir>
        --strict <verify.csv> [--fast <verify.csv>]... [--owners <native-owners.txt>] [--candidates <csv>]

Writes <out dir>/player_inventory.csv (one row per census member), player_coverage_gap.csv (strict-replayed member
instructions a fast run did not replay, grouped into ranges, with the reason) and player_audit.json (every total the
audit doc quotes, with the input SHA-256s).
"""
from __future__ import annotations

import bisect
import collections
import csv
import hashlib
import json
import pathlib
import re
import sys

from dolrecon import dol, game_map

# A class in the player family: the Player namespace, or a Player*.cpp translation unit's anonymous namespace.
PLAYER_CLASS = re.compile(r"^(Player::|@unnamed@Player)")
# Calls that end a verify segment (the per-call cut): bl/bla (LK=1), bctrl, blrl. The next instruction starts the
# segment the callee's return resumes in.
BCTRL, BLRL = 0x4E800421, 0x4E800021


def sha256(path) -> str:
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def is_call(w: int) -> bool:
    return ((w >> 26) == 18 and (w & 1)) or w in (BCTRL, BLRL) or ((w >> 26) == 16 and (w & 1))


def fp_class(w: int) -> str | None:
    """The numeric hazards the oracle cannot prove or the audit must watch (the interpreter and the native ops share
    the same model for each): estimate, single-precision multiply (Gekko rounds frC; neither engine models it),
    fused multiply-add, paired single arithmetic, conversion to integer."""
    op = w >> 26
    xo5 = (w >> 1) & 31
    if (op == 63 and xo5 == 26) or (op == 59 and xo5 == 24) or (op == 4 and xo5 in (24, 26)):
        return "estimate"
    if op == 59 and xo5 in (25, 28, 29, 30, 31):
        return "single_mul"
    if op == 4 and xo5 in (12, 13, 14, 15, 25, 28, 29, 30, 31):
        return "single_mul"  # ps_muls0/1, ps_madds0/1, ps_mul, ps_msub/madd/nmsub/nmadd
    if op == 63 and xo5 in (28, 29, 30, 31):
        return "fused"
    if op == 4 or op in (56, 57, 60, 61):
        return "paired"
    if op == 63 and ((w >> 1) & 1023) in (14, 15):
        return "fctiw"
    return None


def read_coverage(path: str) -> set[int]:
    out = set()
    for line in open(path, encoding="utf-8"):
        s, e = (int(x, 16) for x in line.split(","))
        out.update(range(s, e, 4))
    return out


def read_rows(path: str) -> dict[int, dict]:
    return {int(r["entry"], 16): r for r in csv.DictReader(open(path, encoding="utf-8"))}


def read_arrivals(path: str) -> dict[int, tuple[int, int]]:
    p = pathlib.Path(path)
    if not p.exists():
        return {}
    return {int(r["address"], 16): (int(r["arrivals"]), int(r["checks"])) for r in csv.DictReader(open(p))}


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    root, census_path, out = pathlib.Path(argv[0]), argv[1], pathlib.Path(argv[2])
    opts = collections.defaultdict(list)
    i = 3
    while i < len(argv):
        opts[argv[i]].append(argv[i + 1])
        i += 2
    out.mkdir(parents=True, exist_ok=True)
    image = dol.load_file(str(root / "pal-v30/main.dol"))
    fn = game_map.load_functions(root)
    census = list(csv.DictReader(open(census_path, encoding="utf-8")))
    members = {int(r["address"], 16): r for r in census if r["category"] != "SHARED_ENGINE_DEPENDENCY"}
    shared = {int(r["address"], 16): r for r in census if r["category"] == "SHARED_ENGINE_DEPENDENCY"}
    starts = sorted(fn)

    # ---- 1. the inventory from the binary: ranges, overlaps, padding, instruction words ----
    integrity = collections.Counter()
    words_of = {}
    for a in sorted(members):
        size = int(fn[a]["size"])
        integrity["size_not_multiple_of_4"] += size % 4 != 0
        i = bisect.bisect_right(starts, a)
        if i < len(starts) and a + size > starts[i]:
            integrity["overlaps_next_function"] += 1
        words = [image.word(a + k) for k in range(0, size - size % 4, 4)]
        # Trailing zero words are alignment padding, not instructions (0x00000000 is not a valid instruction).
        while words and words[-1] == 0:
            words.pop()
            integrity["trailing_zero_padding_words"] += 1
        integrity["interior_zero_words"] += words.count(0)
        words_of[a] = words
        if int(members[a]["size"]) != size:
            integrity["census_size_differs_from_functions_csv"] += 1
    integrity["duplicate_addresses_in_census"] = len(census) - len({r["address"] for r in census})

    # ---- 2. membership evidence, re-graded ----
    def grade(a: int, r: dict) -> str:
        cls = r["class"]
        outside = int(r["callers_outside"] or 0)
        inside = int(r["callers_member"] or 0)
        via_vtable = r["reached_from_player_vtables"] == "1" or r["reached_from_player_vtables"] == "True"
        if cls:
            return "class-player" if PLAYER_CLASS.match(cls) else "class-other"
        if r["evidence"] == "library":
            return "library"
        if inside and not outside:
            return "callers-exclusive"
        if inside and outside:
            return "callers-mixed"
        if via_vtable:
            return "vtable-reach"
        return "address-only"  # link order between classed members, no caller or table evidence

    # ---- 3. coverage by run ----
    # Several strict runs: their union is the strict coverage; the first is the one the fast/strict gap compares with.
    strict_path = opts["--strict"][0]
    strict_cov = set().union(*(read_coverage(p + ".coverage") for p in opts["--strict"]))
    first_strict_cov = read_coverage(strict_path + ".coverage")
    strict_rows = read_rows(strict_path)
    fast = [(p, read_coverage(p + ".coverage"), read_rows(p), read_arrivals(p + ".arrivals")) for p in opts["--fast"]]
    owners = set()
    for p in opts["--owners"]:
        for line in open(p, encoding="utf-8"):
            t = line.split("#")[0].split()
            if t:
                owners.add(int(t[0], 16))
    hooked_strict = {a for a, r in strict_rows.items() if int(r.get("owner_verified") or 0) or int(r.get("owner_mismatches") or 0)}
    candidates = collections.defaultdict(list)
    for p in opts["--candidates"]:
        for r in csv.DictReader(open(p, encoding="utf-8")):
            for t in re.findall(r"[0-9A-Fa-f]{8}", r["address"]):
                candidates[int(t, 16)].append(r)

    rows = []
    totals = collections.defaultdict(collections.Counter)
    for a in sorted(members):
        r = members[a]
        words = words_of[a]
        pcs = [a + 4 * k for k in range(len(words)) if words[k] != 0]
        n = len(pcs)
        s_cov = sum(pc in strict_cov for pc in pcs)
        f_cov = [sum(pc in c for pc in pcs) for _, c, _, _ in fast]
        any_cov = sum(pc in strict_cov or any(pc in c for _, c, _, _ in fast) for pc in pcs)
        g = grade(a, r)
        dyn = int(r["dynamic_calls"] or 0)
        native = a in owners or a in hooked_strict
        cand = candidates.get(a, [])
        if native:
            status = "NATIVE_APPROVED"  # integrated, default ON (see the audit doc for what that approval covers)
        elif cand:
            status = "NATIVE_CANDIDATE_UNVERIFIED"
        elif g in ("class-other", "library"):
            status = "SHARED_ENGINE_DEPENDENCY"
        elif g == "address-only":
            status = "UNRESOLVED_IDENTITY"
        elif dyn == 0 and any_cov == 0:
            status = "UNREACHED_OR_UNTESTED"
        else:
            status = "TRANSLATED_PPC"
        hz = collections.Counter(filter(None, map(fp_class, words)))
        row = {"address": f"0x{a:08X}", "name": r["name"], "class": r["class"], "category": r["category"],
               "census_evidence": r["evidence"], "grade": g, "status": status, "instructions": n,
               "padding_words": int(fn[a]["size"]) // 4 - len(words),
               "dynamic_calls": dyn, "strict_replayed": s_cov,
               **{f"fast{k}_replayed": v for k, v in enumerate(f_cov)},
               "any_replayed": any_cov, "native_owner": int(native), "package_candidate": int(bool(cand)),
               "callers_member": r["callers_member"], "callers_outside": r["callers_outside"],
               "indirect_calls": r["indirect_calls"], **{f"fp_{k}": hz[k] for k in
                                                         ("estimate", "single_mul", "fused", "paired", "fctiw")}}
        rows.append(row)
        for key in ("all", status, "grade:" + g, "category:" + r["category"]):
            t = totals[key]
            t["functions"] += 1
            t["instructions"] += n
            t["strict_replayed"] += s_cov
            t["any_replayed"] += any_cov
            t["executed_functions"] += dyn > 0 or any_cov > 0
            for k, v in enumerate(f_cov):
                t[f"fast{k}_replayed"] += v
    with open(out / "player_inventory.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)

    # ---- 4. the fast/strict gap, per instruction, for the first fast run ----
    gap_summary = collections.Counter()
    gap_rows = []
    if fast:
        fpath, fcov, frows, farr = fast[0]
        by_reason = collections.defaultdict(list)
        for a in sorted(members):
            words = words_of[a]
            for k, wd in enumerate(words):
                pc = a + 4 * k
                if wd == 0 or pc not in first_strict_cov or pc in fcov:
                    continue
                # The segment start this instruction was replayed in under strict: the entry, or the instruction
                # after the nearest preceding call (straight-line approximation; a branch can join from elsewhere).
                j = k
                while j > 0 and not is_call(words[j - 1]):
                    j -= 1
                seg = a + 4 * j
                arr, chk = farr.get(seg, (0, 0))
                fr = frows.get(seg)
                skipped = fr is not None and (int(fr["skipped_device"]) + int(fr["skipped_exit"])) > 0
                sr = strict_rows.get(seg)
                strict_n = int(sr["verified"]) if sr else 0
                if arr == 0 and j > 0:
                    # Fast mode does not cut an unsampled call, so the callee returns inline and the resume point
                    # after it never reaches the dispatcher: post-call code is an arrival only after a sampled call.
                    reason = "post-call continuation, never an arrival (the call before it was not sampled)"
                elif arr == 0:
                    reason = "function entry not arrived at in the fast run"
                elif chk == 0:
                    reason = "reached, never checked (sampled out / over budget)"
                elif skipped and int(fr["verified"]) == 0:
                    reason = "reached, checks skipped by the verifier (device / no return)"
                else:
                    reason = "reached and checked, but the checked arrivals took other paths"
                by_reason[(a, seg, reason)].append((pc, arr, chk, strict_n))
        for (a, seg, reason), pcs in sorted(by_reason.items()):
            gap_summary[reason] += len(pcs)
            gap_summary["category:" + members[a]["category"]] += len(pcs)
            gap_rows.append({"function": f"0x{a:08X}", "name": members[a]["name"], "category": members[a]["category"],
                             "segment_start": f"0x{seg:08X}", "first": f"0x{pcs[0][0]:08X}",
                             "last": f"0x{pcs[-1][0]:08X}", "instructions": len(pcs), "fast_arrivals": pcs[0][1],
                             "fast_checks": pcs[0][2], "strict_segments_verified": pcs[0][3], "reason": reason})
        with open(out / "player_coverage_gap.csv", "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(gap_rows[0]) if gap_rows else ["function"], lineterminator="\n")
            w.writeheader()
            w.writerows(gap_rows)

    # ---- 5. the behaviour states, from ShadowBehavior's factory table (0x80522DE8: 35 records of creator, args
    # creator, 0, inline class name) - the binary's own list, not a count carried over from earlier work ----
    status_of = {int(r["address"], 16): r for r in rows}
    states = []
    for sid in range(35):
        rec = image.word(0x80522DE8 + 4 * sid)
        name = image.read(rec + 12, 96).split(b"\0")[0].decode("latin1")
        short = name.rsplit("::", 1)[-1]
        mine = [r for r in rows if r["class"] in (name, f"Player::Behavior::InitializeArgs{short}")]
        states.append({"id": sid, "class": name, "creator": f"0x{image.word(rec):08X}",
                       "functions": len(mine), "instructions": sum(r["instructions"] for r in mine),
                       "executed_functions": sum(1 for r in mine if r["dynamic_calls"] or r["any_replayed"]),
                       "strict_replayed": sum(r["strict_replayed"] for r in mine),
                       "any_replayed": sum(r["any_replayed"] for r in mine)})
    behaviour_classes = {r["class"] for r in rows if r["class"].startswith("Player::Behavior::")
                         and "InitializeArgs" not in r["class"]}
    not_in_table = sorted(behaviour_classes - {s["class"] for s in states} -
                          {"Player::Behavior::Behavior", "Player::Behavior::Factory"})
    with open(out / "player_states.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(states[0]), lineterminator="\n")
        w.writeheader()
        w.writerows(states)

    # ---- 6. paths only a later run reached, and the gap by status ----
    first_only = {}
    if len(fast) > 1:
        seen = first_strict_cov | fast[0][1]
        for k, (_, cov, _, _) in enumerate(fast[1:], 1):
            fresh = collections.Counter()
            for a in members:
                for idx, wd in enumerate(words_of[a]):
                    pc = a + 4 * idx
                    if wd and pc in cov and pc not in seen:
                        fresh[a] += 1
            first_only[opts["--fast"][k]] = {
                "instructions": sum(fresh.values()), "functions": len(fresh),
                "by_category": dict(collections.Counter({members[a]["category"]: 0 for a in fresh}) +
                                    sum((collections.Counter({members[a]["category"]: n}) for a, n in fresh.items()),
                                        collections.Counter())),
                "top": [[f"0x{a:08X}", members[a]["name"], n] for a, n in fresh.most_common(25)]}
    for g_row in gap_rows:
        gap_summary["status:" + status_of[int(g_row["function"], 16)]["status"]] += g_row["instructions"]

    summary = {
        "inputs": {"main.dol": image.sha256, "functions.csv": sha256(root / "skeleton/functions.csv"),
                   "census": sha256(census_path), "strict": [[p, sha256(p)] for p in opts["--strict"]],
                   "fast": [[p, sha256(p)] for p, *_ in fast], "owners": opts["--owners"],
                   "candidates": opts["--candidates"]},
        "census_rows": len(census), "members": len(members), "shared_engine_dependencies_listed": len(shared),
        "integrity": dict(integrity),
        "totals": {k: dict(v) for k, v in sorted(totals.items())},
        "fast_strict_gap": dict(gap_summary),
        "states": states, "behaviour_classes_not_in_shadow_table": not_in_table,
        "reached_first_by_later_fast_runs": first_only,
        "native_owners_in_members": sorted(f"0x{a:08X}" for a in members if a in owners or a in hooked_strict),
    }
    json.dump(summary, open(out / "player_audit.json", "w", encoding="utf-8", newline="\n"), indent=1)
    print(json.dumps({k: summary[k] for k in ("members", "integrity", "fast_strict_gap", "native_owners_in_members")},
                     indent=1))
    for k in sorted(totals):
        if k == "all" or k.isupper() or k.startswith("grade:"):
            print(k, dict(totals[k]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
