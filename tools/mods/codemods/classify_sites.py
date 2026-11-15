#!/usr/bin/env python3
"""Classify PAL (GUPP8P) code addresses: does a patch there survive the static recompilation?

Two modes (stdlib only, every input is a path):

  build   writes test_data/mods/codemods/pal_code_ownership.json: the function table of the recompiler reduced to the
          facts that decide a patch's fate (owner ranges, pruned functions, lowered sites, jump tables, cave range).
  site    classifies addresses (hex words) or a features file against that json:
            classify_sites.py site --ownership pal_code_ownership.json 0x80498940 0x80174764
            classify_sites.py features --ownership ... --features reloaded_features.json [--out counts.json]

Classes of one patched instruction word (the tree is the lifted build, SHADOW_RENDER_LIFTED=ON, the shipped one):

  translated      the word is inside a function the recompiler translates and no list replaces that instruction:
                  the patch takes effect after a variant recompile.
  lowered-site    the word is a listed lowered site (extract/lift-sites*.txt call site, GX pipe site, platform
                  site, wait-point back edge, psq site, empty variadic leaf). The recompiler compares the DOL word
                  with the list and REFUSES a different word: the variant build stops, loudly.
  native-owner    the function is served by a native C++ handler in every build (extract/native-owners.txt):
                  the body is never emitted; a patch inside it changes nothing.
  lifted-owner    the function is owned only by the lifted renderer tree (extract/native-owners-r2.txt): same effect,
                  in the shipped build.
  pruned          the function is not reachable from any root of the tree (liveness pruning): never emitted.
  unemitted       the address is not in any function row the recompiler accepts (the 0x800034AC-0x800053E0
                  interrupt-vector stubs, padding, rejected rows): never translated, never executed.
  translated-named  as translated, but the function's entry is named by the native runtime (extract/liveness-roots*.txt:
                  an install_hle hook that may decline, a native caller, a compiler intrinsic). The patch is in
                  the emitted body; whether the hook runs first has to be checked in runtime/src (listed, not proven).
  data            the word is in a data section, bss, or beyond the image (heap): not code.
  data-jumptable  a data word inside a recognised switch table (the generator verifies the table).

Everything is derived from the repository's own lists plus the layout file the recompiler wrote for the same tree
(recomp_layout.csv, one row per emitted function). No game bytes are read or stored.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path

CAVE_LO, CAVE_HI = 0x800034AC, 0x800053E0
HEX = re.compile(r"0[xX][0-9A-Fa-f]+")


def first_hex(line: str):
    line = line.split("#", 1)[0].strip()
    if not line:
        return None
    m = HEX.match(line)
    return int(m.group(0), 16) if m else None


def read_functions(path):
    rows = []
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            rows.append((int(r["addr"]), int(r["size"]), r["name"]))
    rows.sort()
    return rows


def read_owner_list(path):
    out = {}
    for line in open(path, encoding="utf-8"):
        a = first_hex(line)
        if a is None:
            continue
        body = line.split("#", 1)[0].split(None, 1)
        out[a] = body[1].strip() if len(body) > 1 else ""
    return out


def read_sites(path, kind):
    """word-address lists: lift sites ('site 0xADDR 0xWORD ...'), gx / platform sites ('0xADDR 0xWORD ...'),
    wait points ('0xBACKEDGE 0xHEAD class'), psq sites ('0xADDR gqr 0x...')."""
    out = {}
    for line in open(path, encoding="utf-8"):
        body = line.split("#", 1)[0].split()
        if not body:
            continue
        if body[0] == "site" and len(body) > 1:
            out[int(body[1], 16)] = kind
        elif HEX.fullmatch(body[0]):
            out[int(body[0], 16)] = kind
    return out


def read_jump_tables(path):
    out = []
    for line in open(path, encoding="utf-8"):
        body = line.split("#", 1)[0].split()
        if len(body) >= 3 and HEX.fullmatch(body[0]):
            out.append((int(body[1], 16), int(body[2], 0)))
    return out


def read_layout(path):
    emitted = set()
    for line in open(path, encoding="utf-8"):
        if line.startswith("0x"):
            emitted.add(int(line.split(",")[0], 16))
    return emitted


def dol_sections(path):
    raw = Path(path).read_bytes()[:0xE4]
    h = struct.unpack(">18I18I18I", raw[:0xD8])
    bss, bss_size, entry = struct.unpack(">3I", raw[0xD8:0xE4])
    secs = [(i, h[18 + i], h[36 + i]) for i in range(18) if h[36 + i]]
    return secs, bss, bss_size


def merge(iv):
    out = []
    for lo, hi in sorted(iv):
        if out and lo <= out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def build(a):
    funcs = read_functions(a.functions)
    owners = read_owner_list(a.owners)
    lifted_owners = read_owner_list(a.lifted_owners)
    emitted = read_layout(a.layout)
    secs, bss, bss_size = dol_sections(a.dol)
    sites = {}
    for p in a.lift_sites:
        sites.update({k: "lift" for k in read_sites(p, "lift")})
    for p, k in ((a.gx_sites, "gx"), (a.platform_sites, "platform"), (a.wait_points, "wait"), (a.psq_sites, "psq"),
                 (a.empty_variadic, "empty-variadic")):
        if p:
            for ad in read_sites(p, k):
                sites.setdefault(ad, k)
    # lifted build: the GX pipe sites are lowered only where the fail list does not name them (kept: all listed
    # sites are treated as lowered; a stale-site refusal needs the word to match, so a patch there is refused either way)
    ranges = []
    owner_rows = set()
    for addr, size, name in funcs:
        if addr in owners:
            ranges.append([addr, size, "native-owner", owners[addr]])
            owner_rows.add(addr)
        elif addr in lifted_owners:
            ranges.append([addr, size, "lifted-owner", lifted_owners[addr]])
            owner_rows.add(addr)
    sizes = {addr: size for addr, size, _ in funcs}
    # function rows OVERLAP (a row per entry point, nested inside its function), so coverage is a union of intervals
    emitted_cov = merge([(ad, ad + sizes[ad]) for ad in emitted if ad in sizes and ad not in owner_rows])
    table_cov = merge([(ad, ad + sz) for ad, sz, _ in funcs])
    pruned_rows = [ad for ad, sz, _ in funcs if ad not in emitted and ad not in owner_rows and not (CAVE_LO <= ad < CAVE_HI)]
    unemitted_rows = [ad for ad, sz, _ in funcs if ad not in emitted and ad not in owner_rows and CAVE_LO <= ad < CAVE_HI]
    tables = [[t, n] for t, n in read_jump_tables(a.jump_tables)]
    roots = set()
    if a.roots:
        for line in open(a.roots, encoding="utf-8"):
            v = first_hex(line)
            if v is not None:
                roots.add(v)
    named = sorted(ad for ad, _, _ in funcs if ad in roots and ad in emitted and ad not in owner_rows)
    doc = {
        "schema": 1,
        "tree": "lifted (SHADOW_RENDER_LIFTED=ON, PAL GUPP8P)",
        "note": "default class of a function row is 'translated'; only exceptions are listed. Addresses are PAL.",
        "functions_rows": len(funcs),
        "cave": [CAVE_LO, CAVE_HI],
        "sections": [[i, ad, sz] for i, ad, sz in secs],
        "bss": [bss, bss_size],
        "owners": ranges,
        "emitted": emitted_cov,
        "table": table_cov,
        "jump_tables": tables,
        "runtime_named": named,
        "sites": [[k, v] for k, v in sorted(sites.items())],
        "counts": {"native-owner": sum(r[2] == "native-owner" for r in ranges),
                   "lifted-owner": sum(r[2] == "lifted-owner" for r in ranges),
                   "pruned_rows": len(pruned_rows), "cave_rows": len(unemitted_rows), "sites": len(sites), "runtime_named": len(named),
                   "emitted_ranges": len(emitted_cov), "table_ranges": len(table_cov),
                   "emitted_rows_in_layout": len(emitted)},
    }
    Path(a.out).write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
    print(json.dumps(doc["counts"]))


class Ownership:
    def __init__(self, path, functions=None):
        d = json.loads(Path(path).read_text(encoding="utf-8"))
        self.d = d
        self.owner_starts = [r[0] for r in d["owners"]]
        self.em_starts = [r[0] for r in d["emitted"]]
        self.tb_starts = [r[0] for r in d["table"]]
        self.sites = {k: v for k, v in d["sites"]}
        self.named = set(d.get("runtime_named", []))
        self.secs = d["sections"]
        self.funcs = functions  # optional sorted [(addr,size,name)]
        self._fstarts = [f[0] for f in functions] if functions else None

    @staticmethod
    def _in(starts, rows, a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and rows[i][0] <= a < rows[i][0] + rows[i][1]:
            return rows[i]
        return None

    @staticmethod
    def _cov(starts, rows, a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and rows[i][0] <= a < rows[i][1]

    def function_of(self, a):
        """The outermost function row covering a (rows nest: one per entry point)."""
        if not self.funcs:
            return None
        i = bisect.bisect_right(self._fstarts, a) - 1
        best = None
        for j in range(i, max(-1, i - 64), -1):
            f = self.funcs[j]
            if f[0] <= a < f[0] + f[1] and (best is None or f[0] < best[0]):
                best = f
        return best

    def in_text(self, a):
        return any(i < 7 and ad <= a < ad + sz for i, ad, sz in self.secs)

    def in_data(self, a):
        return any(i >= 7 and ad <= a < ad + sz for i, ad, sz in self.secs) or \
            (self.d["bss"][0] <= a < self.d["bss"][0] + self.d["bss"][1]) or a >= 0x80000000 and not self.in_text(a)

    def classify(self, a):
        """-> (class, detail)"""
        if not self.in_text(a):
            for t, n in self.d["jump_tables"]:
                if t <= a < t + 4 * n:
                    return "data-jumptable", f"switch table 0x{t:08X} ({n} words)"
            return "data", "data section, bss or heap"
        r = self._in(self.owner_starts, self.d["owners"], a)
        if r:
            return r[2], f"0x{r[0]:08X} {r[3]}"
        if self._cov(self.em_starts, self.d["emitted"], a):
            if a in self.sites:
                return "lowered-site", f"listed {self.sites[a]} site: the generator refuses a different DOL word"
            f = self.function_of(a)
            if f and f[0] in self.named:
                return "translated-named", f"function 0x{f[0]:08X} {f[2]}: entry named by the runtime"
            return "translated", (f"function 0x{f[0]:08X} {f[2]}" if f else "")
        if self.d["cave"][0] <= a < self.d["cave"][1]:
            return "unemitted", "0x800034AC-0x800053E0 interrupt-vector stubs: never translated"
        if self._cov(self.tb_starts, self.d["table"], a):
            f = self.function_of(a)
            return "pruned", f"function row 0x{f[0]:08X} is not emitted: unreachable from the tree's roots" if f else "pruned"
        return "unemitted", "no function row covers this address (padding or an unrecognised range)"


VERDICT = {"translated": "works", "translated-named": "works-unless-hooked", "lowered-site": "build-refused", "native-owner": "ineffective",
           "lifted-owner": "ineffective", "pruned": "ineffective", "unemitted": "ineffective",
           "data": "data-engine", "data-jumptable": "data-engine"}


def addr(s):
    return None if s in (None, "") else int(s, 16)


def features_mode(a):
    own = Ownership(a.ownership, read_functions(a.functions) if a.functions else None)
    feats = json.loads(Path(a.features).read_text(encoding="utf-8"))
    rows = []
    for ft in feats["features"]:
        for ent in ft["library_entries"]:
            for p in ent["patches"]:
                pal = addr(p.get("pal"))
                cls, det = (own.classify(pal) if pal is not None else ("unmapped", "no PAL address"))
                if p["kind"] == "data" and cls not in ("data", "data-jumptable") and pal is not None:
                    cls_note = "listed as data by the map"
                rows.append({"source": "mcm", "feature": ft["id"], "entry": ent["entry"], "ntsc": p["ntsc"], "pal": p.get("pal"),
                             "patch_kind": p["kind"], "needs_cave": p["kind"] == "branch-to-cave", "class": cls,
                             "detail": det})
    for g in feats["gecko"]:
        for w in g["writes"]:
            pal = addr(w.get("pal"))
            cls, det = (own.classify(pal) if pal is not None else ("unmapped", w.get("region", "no PAL address")))
            rows.append({"source": "gecko", "feature": g["name"], "entry": g["name"], "ntsc": w["ntsc"], "pal": w.get("pal"),
                         "patch_kind": w["kind"], "needs_cave": False, "class": cls, "detail": det})
    counts = {}
    for src in ("mcm", "gecko"):
        c = Counter(r["class"] for r in rows if r["source"] == src)
        counts[src] = dict(c.most_common())
        counts[src]["_total"] = sum(c.values())
    # per entry (an MCM library entry / one Gecko code): worst class of its code writes; data-only entries are 'data'
    rank = ["lowered-site", "native-owner", "lifted-owner", "pruned", "unemitted", "unmapped", "translated-named", "translated",
            "data-jumptable", "data"]
    per_entry = {}
    for r in rows:
        per_entry.setdefault((r["source"], r["entry"]), []).append(r["class"])
    entry_counts = {}
    for (src, _), cl in per_entry.items():
        worst = min(cl, key=rank.index)
        entry_counts.setdefault(src, Counter())[worst] += 1
    by_kind = Counter((r["source"], r["patch_kind"], r["class"]) for r in rows)
    out = {"counts": counts, "entries_by_worst_class": {k: dict(v.most_common()) for k, v in entry_counts.items()},
           "by_patch_kind": {"|".join(k): v for k, v in sorted(by_kind.items())},
           "cave_patches_by_site_class": dict(Counter(r["class"] for r in rows if r["needs_cave"])), "rows": rows}
    if a.out:
        Path(a.out).write_text(json.dumps(out, indent=1), encoding="utf-8")
    print(json.dumps({"counts": counts, "entries_by_worst_class": out["entries_by_worst_class"], "by_patch_kind": out["by_patch_kind"],
                      "cave_patches_by_site_class": out["cave_patches_by_site_class"]}, indent=1))


def site_mode(a):
    own = Ownership(a.ownership, read_functions(a.functions) if a.functions else None)
    for s in a.addresses:
        cls, det = own.classify(int(s, 16))
        print(f"{s}\t{cls}\t{VERDICT.get(cls, '?')}\t{det}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--functions", required=True)
    b.add_argument("--owners", required=True)
    b.add_argument("--lifted-owners", required=True)
    b.add_argument("--layout", required=True, help="recomp_layout.csv the recompiler wrote for this tree")
    b.add_argument("--dol", required=True, help="main.dol: only the section table is read")
    b.add_argument("--lift-sites", action="append", default=[])
    b.add_argument("--gx-sites")
    b.add_argument("--platform-sites")
    b.add_argument("--wait-points")
    b.add_argument("--psq-sites")
    b.add_argument("--empty-variadic")
    b.add_argument("--roots", help="liveness-roots-lifted.txt: guest entries the runtime names")
    b.add_argument("--jump-tables", required=True)
    b.add_argument("--out", required=True)
    s = sub.add_parser("site")
    s.add_argument("--ownership", required=True)
    s.add_argument("--functions")
    s.add_argument("addresses", nargs="+")
    f = sub.add_parser("features")
    f.add_argument("--ownership", required=True)
    f.add_argument("--functions")
    f.add_argument("--features", required=True)
    f.add_argument("--out")
    a = ap.parse_args()
    {"build": build, "site": site_mode, "features": features_mode}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
