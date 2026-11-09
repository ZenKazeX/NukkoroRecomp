"""Name the game's RenderWare functions from the RenderWare 3.7 PC static libraries (same sources, other compiler).

    python -B -m dolrecon.rwalign <named project> <spine dir> <out dir> [--holdout 0.33]      (from source/tools)

Reference: the NULL-device release libraries in RW37SDK ("Library NULL lib Files/release"): every RenderWare
function with its source module, its position in that module, its size, the functions it calls and whose address it
takes (in order), the globals it references, the strings it uses and its code bytes.
Seeds: the STRONG anchors of RW37SDK/_mined/rw_guest_naming_candidates.csv, PROVEN (behaviour: rwcore oracle), and
the RenderWare API names of <out>/rw_driver.json (dolrecon.rwdriver); its other, GameCube-only names mark driver code.

Rules, applied to a fixed point; a guest function gets a name only when every vote it receives agrees, the name is
not claimed by another guest function, and the guest/PC size ratio is plausible (0.5..2.5):
  C1 calls      ordered callee lists of a named pair, cut at callees known on both sides: a gap of exactly ONE
                function on each side pairs (longer gaps can be ordered differently on the two platforms), when both
                have the same callee count and no other function of the PC parent's module calls the PC element
                (either compiler may have inlined a same-module function: gap_ok).
  C4 calls      longer gaps of equal length whose position-by-position pairing is consistent, agreed by >= 3 callers
                (_rwerror / RwErrorSet at every RWERROR site).
  F1 pointers   the one-gap rule for the ordered lists of functions whose ADDRESS a named pair takes (callbacks).
  T1 tables     data tables paired (same string entries, a mapped global, or known functions at the same offsets;
                same size; every offset consistent): their function pointers pair slot by slot (32-bit on both
                sides: the same struct has the same layout).
  O1 order      between two named functions of one module, kept in source order in the guest, the functions in
                between pair when the counts are equal AND every pair agrees in callee count and size.
  O2 order      the same when the PC module has up to O2_SPARE more functions there (the guest linker strips unused
                ones): the guest run must embed in order into the PC run in exactly ONE compatible way.
  S1 string     the same string literal, used by exactly one function on each side.
  K1 constant   the same distinctive constants (values >= 0x100 that are no address, each used by <= 2 functions on
                each side; PC: found as a 32-bit immediate in the code): all of a guest function's distinctive
                constants are in exactly one unused PC function.
  C2/C3/D2      constraint intersection: the twin must be called (or have its address taken) by the twins of ALL
                known callers, call the twins of ALL known callees, and reference ALL mapped globals; at least two
                different known neighbours, one of them no hub; call-graph evidence: one candidate left after the
                size / callee filters and no OTHER guest function fits it seen from the PC side (mutual); globals
                alone: unique before the filters.
  D1 globals    globals are mapped too: when a named pair references exactly one global not yet mapped on each side,
                those two globals are the same (unanimous, one-to-one); T1 maps data pointers of paired tables.
Every accepted name passes:
  E1 existence  no guest function that calls GameCube SDK code is named (driver code has no NULL-library twin).
  E2 locality   not inside another library's block of names, nor away from its own library's names.
  consistency   no known neighbour contradicts it (a call missing on one side is explained only by inlining:
                inline-sized or same-module callee, one level deep); a contradiction keeps the better-supported side.
--holdout hides that fraction of the seeds, runs, and reports how many hidden seeds came back right / wrong (also:
their name given to another function) / not at all. Independent checks: dolrecon.rwcheck (field offsets, link order);
behaviour (rwcore oracle tests) remains the final check.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import os
import random
import struct
import sys

from . import coff
from . import elfobj
from . import match as mmatch
from . import strrefs

SDK = r"H:\ShadowTheHedgehogDiskGC_Extract\RW37SDK"
LIBDIR = os.path.join(SDK, "Library NULL lib Files", "release")
SEEDS = os.path.join(SDK, "_mined", "rw_guest_naming_candidates.csv")
# Proven by behaviour: the native source/rwcore function is bit-identical to the original instructions over 20,000
# oracle cases each (runtime/tests/rwcore_matrix_tests.cpp, with a failing negative control). Stronger than any seed.
PROVEN = {0x8047FC08: "RwMatrixMultiply", 0x8047FBF0: "RwMatrixUpdate", 0x804834A8: "RwV3dTransformPoint"}
C4_MIN = 3
MIN_NEIGHBOURS = 2
HUB = 25
O2_SPARE = 8
STATIC_PREFIXES = ("$", "__real@", "_real@", "_xmm@", "??_C@", ".data", ".rdata", ".bss", ".text", "__fltused")


def load_pc():
    funcs, order = {}, collections.defaultdict(list)
    for lib in sorted(os.listdir(LIBDIR)):
        if not lib.endswith(".lib"):
            continue
        for f in coff.library_functions(os.path.join(LIBDIR, lib)):
            mod = f.module.replace("\\", "/").split("/")[-1]
            f.module = f"{lib[:-4]}/{mod}"
            if f.name in funcs:
                continue
            funcs[f.name] = f
            order[f.module].append(f)
    for mod, lst in order.items():
        lst.sort(key=lambda f: (f.section, f.offset))
    for f in funcs.values():
        f.fptrs = [r for r in f.refs if r in funcs]
        f.data = {r for r in f.refs if r not in funcs and not r.startswith(STATIC_PREFIXES)}
    return funcs, {m: [f.name for f in lst] for m, lst in order.items()}


def load_pc_data():
    """PC data objects keyed 'module|name' (static names repeat across modules), each (size, entries)."""
    objs = {}
    for lib in sorted(os.listdir(LIBDIR)):
        if lib.endswith(".lib"):
            for d in coff.library_data(os.path.join(LIBDIR, lib)):
                objs.setdefault(f"{lib[:-4]}/{d.module.split('/')[-1]}|{d.name}", (d.size, d.entries))
    return objs


def guest_constants(proj, addr: int, size: int) -> set[int]:
    """Distinctive-constant candidates: li / cmpwi / cmplwi immediates >= 0x100 and lis+ori / lis+addi values that
    are no address in the game's memory."""
    out, hi = set(), {}
    for a in range(addr, addr + size, 4):
        raw = proj.read(a, 4)
        if raw is None:
            break
        w = struct.unpack(">I", raw)[0]
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        imm, simm = w & 0xFFFF, (w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)
        if op == 15 and ra == 0:
            hi[rd] = imm << 16
            continue
        if op == 14 and ra == 0 and abs(simm) >= 0x100:
            out.add(simm & 0xFFFFFFFF)
        elif op in (10, 11) and (imm if op == 10 else abs(simm)) >= 0x100:
            out.add((imm if op == 10 else simm) & 0xFFFFFFFF)
        elif op in (24, 14) and ra in hi and (op == 24 and rd in hi or op == 14):
            base = hi[rd if op == 24 else ra]
            v = (base | imm) if op == 24 else (base + simm) & 0xFFFFFFFF
            if not 0x80000000 <= v < 0x81800000 and v >= 0x10000:
                out.add(v)
        if op not in (15,):
            hi.pop(rd if op != 24 else ra, None)
    return out


def load_guest(project: str, spine: str):
    with open(os.path.join(spine, "functions.csv"), encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    info = {int(r["address"], 16): r for r in rows}
    addr_of = {r["name"]: a for a, r in info.items()}
    calls, fptrs = collections.defaultdict(list), collections.defaultdict(list)
    gobjs = {}                                    # guest data object -> (size, {offset: entry}), entries as PC side
    strings, data = collections.defaultdict(list), collections.defaultdict(set)
    proj = mmatch.Project(project)
    pool_text, pooled, pool_objects = strrefs.project_refs(project)

    def cstr(addr):
        out = bytearray()
        while len(out) < 256:
            b = proj.read(addr + len(out), 1)
            if not b or b == b"\0":
                break
            out += b
        try:
            t = out.decode("ascii")
        except UnicodeDecodeError:
            return None
        return t if len(t) >= 4 and t.isprintable() else None

    obj_dir = os.path.join(project, "build", "obj")
    for n in os.listdir(obj_dir):
        o = elfobj.read(os.path.join(obj_dir, n))
        secs = {sec.index: sec for sec in o.sections}
        dsyms = collections.defaultdict(list)
        for s in o.symbols:
            if (s.defined and s.size and s.type in (elfobj.STT_OBJECT, elfobj.STT_NOTYPE) and s.shndx in secs
                    and not secs[s.shndx].flags & 4):
                dsyms[s.shndx].append((s.value, s.size, s.name))
        for sh, lst in dsyms.items():
            lst.sort()
            offs = [v for v, _, _ in lst]
            for r in o.relocs.get(sh, ()):
                i = bisect.bisect_right(offs, r.offset) - 1
                if i < 0 or r.type != 1 or r.offset >= lst[i][0] + lst[i][1]:
                    continue
                v, size, oname = lst[i]
                tgt = r.symbol.name
                if tgt in addr_of and r.addend == 0:
                    e = ("fn", addr_of[tgt])
                elif tgt in proj.symbols:
                    t = cstr(proj.symbols[tgt].address + r.addend)
                    e = ("str", t) if t else ("data", tgt, r.addend)
                else:
                    continue
                gobjs.setdefault(oname, (size, {}))[1][r.offset - v] = e
        fs = sorted((s.shndx, s.value, addr_of[s.name]) for s in o.symbols
                    if s.type == elfobj.STT_FUNC and s.defined and s.name in addr_of)
        for sh, rs in o.relocs.items():
            sf = [(v, a) for h, v, a in fs if h == sh]
            if not sf:
                continue
            offs = [v for v, _ in sf]
            for r in rs:
                i = bisect.bisect_right(offs, r.offset) - 1
                if i < 0:
                    continue
                f, name = sf[i][1], r.symbol.name
                if r.type == 10 and name in addr_of:
                    calls[f].append((r.offset, addr_of[name]))
                elif r.type in (4, 109) and name in addr_of:
                    fptrs[f].append((r.offset, addr_of[name]))
                elif r.type in (4, 109) and name in pool_objects:
                    continue                    # a string pool: its strings come from dolrecon.strrefs below
                elif r.type in (4, 109) and name in proj.symbols:
                    t = cstr(proj.symbols[name].address + r.addend)
                    if t:
                        strings[f].append(t)
                    elif not name.startswith(("kF32_", "kF64_", "str_")):
                        data[f].add(name)
    # strings reached through a pool's register offset (a relocation names only the pool; reading at its address
    # gave every user the pool's FIRST string - wrong attributions until 2026-11-01)
    for f, rs in pooled.items():
        if f in info:
            strings[f].extend(pool_text[a] for _, a in rs)
    ordered = {f: [t for _, t in sorted(c)] for f, c in calls.items()}
    optrs = {f: [t for _, t in sorted(c)] for f, c in fptrs.items()}
    rw = {a for a, r in info.items() if a >= 0x80440000 and r["layer"] in ("rw", "unresolved")}
    consts = {a: guest_constants(proj, a, int(info[a]["size"])) for a in rw}
    return info, ordered, rw, strings, optrs, data, consts, gobjs


def one_gaps(G, P, known, longer=None):
    """Pairs from ordered lists cut at elements known on both sides; only one-element gaps pair. None if a known
    element of G is in P but out of order (the lists disagree); one absent from P (inlined there) only voids its gap. With `longer` (a list), gaps of equal length >= 2
    whose position-by-position pairing is consistent (each element always beside the same partner, one-to-one) are
    appended to it as (element, partner, gap): rule C4 accepts them only when several callers agree."""
    cut_g, cut_p, j = [-1], [-1], 0
    for i, c in enumerate(G):
        if c in known:
            try:
                j = P.index(known[c][0], j)
            except ValueError:
                if known[c][0] in P:
                    return None                  # present, but out of order: the lists disagree
                continue                         # absent (inlined on PC): no cut; its gap holds a known element
                                                 # and so pairs nothing
            cut_g.append(i)
            cut_p.append(j)
            j += 1
    cut_g.append(len(G))
    cut_p.append(len(P))
    out = []
    for k in range(len(cut_g) - 1):
        sg, sp = G[cut_g[k] + 1:cut_g[k + 1]], P[cut_p[k] + 1:cut_p[k + 1]]
        if len(sg) == 1 and len(sp) == 1 and sg[0] not in known:
            out.append((sg[0], sp[0], k))
        elif longer is not None and len(sg) == len(sp) >= 2 and not any(c in known for c in sg):
            fw, bw = {}, {}
            if all(fw.setdefault(a, b) == b and bw.setdefault(b, a) == a for a, b in zip(sg, sp)):
                longer.extend((a, b, k) for a, b in fw.items())
    return out


def table_pairs(gobjs, pobjs, known, dmap, rwset):
    """T1: pair guest data tables with PC data tables, then their entries offset by offset (both 32-bit: the same
    struct has the same layout). A table pair comes from (a) the same set of (offset, string) entries, unique on
    both sides, (b) a mapped global (D1), or (c) known functions at the same offsets, one PC table only.
    Every pair must agree with what is already known at every offset (a function where the other side has a string
    or another known function rejects it). Returns function votes and global votes."""
    pc_name = collections.defaultdict(list)
    for k in pobjs:
        pc_name[k.split("|", 1)[1]].append(k)
    fn_at = collections.defaultdict(set)
    for k, (_, e) in pobjs.items():
        for off, x in e.items():
            if x[0] == "fn":
                fn_at[(off, x[1])].add(k)

    def strsig(e):
        return frozenset((off, x[1]) for off, x in e.items() if x[0] == "str")
    pairs = collections.defaultdict(set)
    gs, ps = collections.defaultdict(list), collections.defaultdict(list)
    for k, (_, e) in gobjs.items():
        if strsig(e):
            gs[strsig(e)].append(k)
    for k, (_, e) in pobjs.items():
        if strsig(e):
            ps[strsig(e)].append(k)
    for q, gl in gs.items():
        if len(gl) == 1 and len(ps.get(q, ())) == 1:
            pairs[gl[0]].add(ps[q][0])
    for gname, pname in dmap.items():
        if gname in gobjs and len(pc_name.get(pname, ())) == 1:
            pairs[gname].add(pc_name[pname][0])
    for k, (_, e) in gobjs.items():
        kf = [(off, known[x[1]][0]) for off, x in e.items() if x[0] == "fn" and x[1] in known]
        if not kf:
            continue
        cand = None
        for key in kf:
            cand = set(fn_at.get(key, ())) if cand is None else cand & fn_at.get(key, set())
        if len(cand) == 1:
            pairs[k].add(next(iter(cand)))
    claimed = collections.Counter(next(iter(v)) for v in pairs.values() if len(v) == 1)
    fvotes, dvotes = [], []
    for gk, cands in pairs.items():
        if len(cands) != 1:
            continue
        pk = next(iter(cands))
        if claimed[pk] > 1:
            continue
        (gsize, ge), (psize, pe) = gobjs[gk], pobjs[pk]
        if gsize != psize:
            continue
        ok, fv, dv = True, [], []
        for off in set(ge) & set(pe):
            a, b = ge[off], pe[off]
            if a[0] != b[0]:
                ok = False
            elif a[0] == "fn":
                if a[1] in known:
                    ok &= known[a[1]][0] == b[1]
                elif a[1] in rwset:
                    fv.append((a[1], b[1], f"T1 {pk.split('|', 1)[1][:60]}+0x{off:X}"))
            elif a[0] == "str":
                ok &= a[1] == b[1]
            elif a[2] == b[2] and not b[1].startswith("."):
                dv.append((a[1], b[1]))
        for off in set(ge) ^ set(pe):                 # a pointer on one side only: different layouts
            ok &= (ge.get(off) or pe.get(off))[0] == "data"
        if ok:
            fvotes += fv
            dvotes += dv
    return fvotes, dvotes


def run(pc, pc_order, info, g, seeds, ratio_lo, ratio_hi, gobjs=None, pobjs=None, trace=(), more_driver=()):
    gcalls, rwset, gstrings, gptrs, gdata, gconsts = g
    known = dict(seeds)                                   # guest address -> (pc name, rule, evidence)
    used = {p for p, _, _ in known.values()}
    dmap, dused = {}, set()                               # guest global -> PC global
    pos = {name: (m, i) for m, names in pc_order.items() for i, name in enumerate(names)}
    gcallers = collections.defaultdict(set)
    for f, cs in list(gcalls.items()) + list(gptrs.items()):
        for c in cs:
            gcallers[c].add(f)
    pc_callers = collections.defaultdict(set)
    for p, f in pc.items():
        for c in f.calls + f.fptrs:
            pc_callers[c].add(p)
    pc_data_users = collections.defaultdict(set)
    for p, f in pc.items():
        for d in f.data:
            pc_data_users[d].add(p)
    pc_string_users = collections.defaultdict(list)
    for p, f in pc.items():
        for t in set(f.strings):
            pc_string_users[t].append(p)
    guest_string_users = collections.defaultdict(set)
    for x, texts in gstrings.items():
        for t in texts:
            guest_string_users[t].add(x)
    # distinctive constants: rare on both sides
    gconst_users = collections.defaultdict(set)
    for x, cs in gconsts.items():
        for c in cs:
            gconst_users[c].add(x)
    all_consts = set(gconst_users)
    pc_const_users = collections.defaultdict(set)
    for p, f in pc.items():
        code = f.code
        for c in all_consts:
            if struct.pack("<I", c) in code:
                pc_const_users[c].add(p)
    distinctive = {c for c in all_consts if 1 <= len(gconst_users[c]) <= 2 and 1 <= len(pc_const_users[c]) <= 2}
    rw_sorted = sorted(rwset)
    banned, withdrawn = set(), {}
    module_funcs = collections.defaultdict(list)
    for name, fn in pc.items():
        module_funcs[fn.module].append(name)
    # E1 existence: a guest function that calls GameCube SDK code (GX, OS, ...) is device-driver code; the NULL
    # libraries have no twin for it, so whatever candidate the rules leave would be wrong (6 such names, 2026-11-01)
    driver = {x for x in rwset if any(info.get(c, {}).get("layer") == "sdk" for c in gcalls.get(x, ()))}
    driver |= set(more_driver) - set(seeds)      # named by role from the driver tables (dolrecon.rwdriver)
    reach = {p: set(c for c in pc[p].calls + pc[p].fptrs if c in pc) for p in pc}
    # evidence weight: how many PC functions a neighbour constrains (users of a callee / global, callees of a caller)
    degree = collections.Counter()
    for fn in pc.values():
        for n in set(fn.calls) | set(fn.fptrs) | set(fn.data):
            degree[n] += 1
    for p in pc:
        degree[p] = max(degree[p], len(reach[p]))

    def n_calls_g(x):
        return len([c for c in gcalls.get(x, []) if c in rwset])

    def n_calls_p(y):
        return len([c for c in pc[y].calls if c in pc])

    gdata_users = collections.defaultdict(set)
    for x0, ds in gdata.items():
        for d in ds:
            gdata_users[d].add(x0)

    def reverse(y):
        """Guest functions (not yet named, not driver) that fit the candidate y as seen from the PC side."""
        twin = {p: x0 for x0, (p, _, _) in known.items()}
        dtwin = {pd: gd for gd, pd in dmap.items()}
        cand = None
        for c in pc_callers.get(y, ()):
            if c in twin:
                got = set(gcalls.get(twin[c], ())) | set(gptrs.get(twin[c], ()))
                cand = got if cand is None else cand & got
        for d in pc[y].calls + pc[y].fptrs:
            if d in twin:
                cand = set(gcallers.get(twin[d], ())) if cand is None else cand & gcallers.get(twin[d], set())
        for d in pc[y].data:
            if d in dtwin:
                cand = set(gdata_users.get(dtwin[d], ())) if cand is None else cand & gdata_users.get(dtwin[d], set())
        return {x0 for x0 in (cand or ()) if x0 in rwset and x0 not in known and x0 not in driver
                and size_ok(x0, y) and n_calls_g(x0) == n_calls_p(y)}

    def gap_ok(a, b, parent):
        """A call-gap pair survives inlining checks: (1) same callee count (CodeWarrior inlined a same-module callee
        b of the PC parent: the guest calls b's callees instead - RwStreamFindChunk / _rwStreamReadChunkHeader);
        (2) no unnamed function of the PC parent's module, other than b, calls b (MSVC inlined it: the PC calls b
        where the guest calls that function - RpAtomicCreate inlines RpAtomicSetFrame, found 2026-11-01)."""
        if n_calls_g(a) != n_calls_p(b):
            return False
        mod = pc[parent].module
        return not any(q != b and q != parent and q not in used and b in pc[q].calls
                       for q in module_funcs.get(mod, ()))

    def embed(gb, pb):
        """O2: the guest functions between two named functions of one module, in order, as a subsequence of the PC
        module's functions there (the guest linker strips unused ones). Pairs only when exactly ONE order-preserving
        embedding has every pair compatible (callee count, size, unused, no contradicting known neighbour)."""
        twin = {k: v[0] for k, v in known.items()}

        def compat(a, b):
            return (b not in used and n_calls_g(a) == n_calls_p(b) and size_ok(a, b)
                    and not consistency(pc, gcalls, gptrs, gcallers, twin, a, b)[1])
        n, m = len(gb), len(pb)
        ok = [[compat(gb[i], pb[j]) for j in range(m)] for i in range(n)]
        ways = [[0] * (m + 1) for _ in range(n + 1)]
        for j in range(m + 1):
            ways[0][j] = 1
        for i in range(1, n + 1):
            for j in range(1, m + 1):
                ways[i][j] = min(2, ways[i][j - 1] + (ways[i - 1][j - 1] if ok[i - 1][j - 1] else 0))
        if ways[n][m] != 1:
            return []
        out, i, j = [], n, m
        while i:
            if ok[i - 1][j - 1] and ways[i - 1][j - 1] == 1 and ways[i][j - 1] == 0:
                out.append((gb[i - 1], pb[j - 1]))
                i, j = i - 1, j - 1
            else:
                j -= 1
        return out

    def size_ok(x, y):
        gs, ps = int(info[x]["size"]), pc[y].size
        return ps > 0 and ratio_lo <= gs / ps <= ratio_hi

    while True:
        votes = collections.defaultdict(lambda: collections.defaultdict(list))
        dvotes = collections.defaultdict(lambda: collections.defaultdict(int))
        c4 = collections.defaultdict(lambda: collections.defaultdict(set))
        for x, (p, _, _) in list(known.items()):
            # C1 / F1 (C4: longer gaps, collected per parent)
            for G, P, tag in (([c for c in gcalls.get(x, []) if c in rwset], [c for c in pc[p].calls if c in pc], "C1"),
                              ([c for c in gptrs.get(x, []) if c in rwset], pc[p].fptrs, "F1")):
                if G and P:
                    longer = []
                    for a, b, k in one_gaps(G, P, known, longer if tag == "C1" else None) or []:
                        if tag == "F1" or gap_ok(a, b, p):
                            votes[a][b].append(f"{tag} {k} of {p}")
                    for a, b, k in longer:
                        if gap_ok(a, b, p):
                            c4[a][b].add(p)
            # D1: one unmapped global on each side
            gd = {d for d in gdata.get(x, ()) if d not in dmap}
            pd = {d for d in pc[p].data if d not in dused}
            if len(gd) == 1 and len(pd) == 1:
                dvotes[next(iter(gd))][next(iter(pd))] += 1
        # C4: the same position-by-position pairing in at least C4_MIN different known callers
        for a, cands in c4.items():
            for b, parents in cands.items():
                if len(parents) >= C4_MIN or len(cands) > 1:
                    votes[a][b].append(f"C4 {len(parents)} callers ({', '.join(sorted(parents)[:2])})")
        if gobjs is not None:
            tf, td = table_pairs(gobjs, pobjs, known, dmap, rwset)
            for a, b, why in tf:
                if a not in known and b in pc:
                    votes[a][b].append(why)
            for a, b in td:
                if a not in dmap and b not in dused:
                    dvotes[a][b] += 1
        for gd, cands in dvotes.items():
            if len(cands) == 1:
                pd = next(iter(cands))
                if pd not in dused and sum(1 for v in dvotes.values() if pd in v) == 1:
                    dmap[gd] = pd
                    dused.add(pd)
        # O1
        by_mod = collections.defaultdict(list)
        for x, (p, _, _) in known.items():
            if p in pos:
                m, i = pos[p]
                by_mod[m].append((x, i))
        for m, lst in by_mod.items():
            lst.sort()
            for (g1, i1), (g2, i2) in zip(lst, lst[1:]):
                if i2 <= i1:
                    continue
                gb = rw_sorted[bisect.bisect_right(rw_sorted, g1):bisect.bisect_left(rw_sorted, g2)]
                pb = pc_order[m][i1 + 1:i2]
                if gb and len(gb) == len(pb) and all(n_calls_g(a) == n_calls_p(b) and size_ok(a, b)
                                                     for a, b in zip(gb, pb)):
                    for a, b in zip(gb, pb):
                        if a not in known:
                            votes[a][b].append(f"O1 {m} between {known[g1][0]} and {known[g2][0]}")
                elif (gb and len(gb) < len(pb) <= len(gb) + O2_SPARE and not any(a in known for a in gb)
                      and not any(a in driver for a in gb)):
                    for a, b in embed(gb, pb):
                        votes[a][b].append(f"O2 {m} between {known[g1][0]} and {known[g2][0]} "
                                           f"({len(gb)} of {len(pb)})")
        for x in rwset:
            if x in known or x in driver:
                continue
            # S1
            for t in gstrings.get(x, ()):
                ys = pc_string_users.get(t, [])
                if len(ys) == 1 and len(guest_string_users[t]) == 1:
                    votes[x][ys[0]].append(f"S1 string {t[:40]!r}")
            # K1
            ks = [c for c in gconsts.get(x, ()) if c in distinctive]
            if ks:
                cand = None
                for c in ks:
                    cand = set(pc_const_users[c]) if cand is None else cand & pc_const_users[c]
                # uniqueness BEFORE the size / callee filters: a filter that drops the true twin (inlining changes
                # callee counts) must not leave a wrong candidate "unique" (found 2026-10-31 with D2)
                cand = {y for y in cand if y not in used}
                y = next(iter(cand)) if len(cand) == 1 else None
                if y and (len(ks) >= 2 or max(ks) >= 0x10000) and size_ok(x, y) and n_calls_g(x) == n_calls_p(y):
                    votes[x][y].append(f"K1 constants {', '.join(hex(c) for c in ks[:3])}")
            # C2 / C3 / D2
            kc = [known[c][0] for c in gcallers.get(x, ()) if c in known]
            kd = [known[d][0] for d in gcalls.get(x, []) + gptrs.get(x, []) if d in known]
            kg = [dmap[d] for d in gdata.get(x, ()) if d in dmap]
            # at least two different known neighbours (callers, callees, mapped globals): with one, a twin whose copy
            # of that neighbour was inlined by either compiler is invisible (RpLightDestroy inlined into
            # LightTidyDestroyLight on PC, DestroyClumpLight into RpClumpDestroy on GameCube, 2026-11-01)
            # ... and at least one of them no hub (RwErrorSet, RwEngineInstance, ...: more than HUB PC functions use
            # them; RtTOCCreate was named from "RwErrorSet, RwEngineInstance" alone, 2026-11-01)
            near = set(kc) | set(kd) | set(kg)
            if len(near) < MIN_NEIGHBOURS or all(degree.get(n, 0) > HUB for n in near):
                continue
            cand = None
            for c in kc:
                cand = set(reach.get(c, ())) if cand is None else cand & reach.get(c, set())
            for d in kd:
                cand = set(pc_callers.get(d, ())) if cand is None else cand & pc_callers.get(d, set())
            for d in kg:
                cand = set(pc_data_users.get(d, ())) if cand is None else cand & pc_data_users.get(d, set())
            cand = {y for y in (cand or ()) if y not in used}
            if kc or kd:
                # call-graph evidence: unique after the size / callee filters AND mutual: searched back from the PC
                # side (guest functions with the twins of the candidate's known PC neighbours), only x may remain.
                # One-sided filtered uniqueness named _rwerror "RwErrorSet": the size filter dropped the true twin
                # (CodeWarrior's variadic prologue makes _rwerror 6.5x its PC size), found 2026-11-01.
                raw = cand
                cand = {y for y in cand if size_ok(x, y) and n_calls_p(y) == n_calls_g(x)}
                if x in trace:
                    print(f"trace 0x{x:08X} C2/C3: callers {kc[:4]} callees {kd[:4]} globals {kg[:3]}; raw {sorted(raw)[:6]}"
                          f" filtered {sorted(cand)[:4]} reverse {[hex(r) for r in reverse(next(iter(cand)))] if len(cand) == 1 else '-'}")
                if len(cand) == 1 and reverse(next(iter(cand))) - {x}:
                    cand = set()                  # another guest function fits the candidate as well
            # globals alone: unique BEFORE the filters (filtered uniqueness named two functions wrongly, 2026-10-31)
            y = next(iter(cand)) if len(cand) == 1 else None
            if y and size_ok(x, y) and n_calls_p(y) == n_calls_g(x):
                tags = ("C2 " if kc else "") + ("C3 " if kd else "") + ("D2 " if kg else "")
                votes[x][y].append(tags + ", ".join((kc + kd + kg)[:3]))
        for t in trace:
            if t in votes and t not in known:
                print(f"trace 0x{t:08X}: " + "; ".join(f"{b} <- {v[:2]}" for b, v in votes[t].items()))
        added = 0
        claimed = collections.Counter(next(iter(v)) for v in votes.values() if len(v) == 1)
        for x, cand in votes.items():
            if len(cand) != 1 or x in known or x in driver:
                continue
            p, why = next(iter(cand.items()))
            if p in used or claimed[p] > 1 or not size_ok(x, p) or (x, p) in banned:
                continue
            # E2 link locality: CodeWarrior links each library contiguously; a name whose library differs from those
            # of both nearest named guest neighbours is an island (_rpCollTreeStreamReadPre36001, a collision-plugin
            # name, landed amid rwcore stream code, 2026-11-01)
            # (inside another library's block, or away from the names its own library already has; the first name
            # of a library may sit at any boundary)
            ks = sorted(known)
            i = bisect.bisect_left(ks, x)
            lib = pc[p].module.split("/")[0]
            if 0 < i < len(ks):
                lp, ln = (pc[known[ks[i - 1]][0]].module.split("/")[0], pc[known[ks[i]][0]].module.split("/")[0])
                if lib not in (lp, ln) and (lp == ln or any(pc[v[0]].module.startswith(lib + "/")
                                                            for v in known.values())):
                    continue
            # consistency with every known neighbour; a contradiction is settled by support: the side with more
            # agreeing neighbours stays, the other pair is banned (seeds always stay). Found 2026-11-01: C3 named
            # LightTidyDestroyLight "DestroyClumpLight" (twin bodies), contradicted by _rpLightClose's pointer.
            twin = {k: v[0] for k, v in known.items()}
            agree, hard = consistency(pc, gcalls, gptrs, gcallers, twin, x, p)
            if hard:
                rivals = {c for c, _ in hard}
                if any(known[c][1] == "S0" or consistency(pc, gcalls, gptrs, gcallers, twin, c, twin[c])[0] >= agree
                       for c in rivals):
                    banned.add((x, p))
                    continue
                for c in rivals:
                    banned.add((c, twin[c]))
                    used.discard(twin[c])
                    withdrawn[c] = (twin[c], f"withdrawn: contradicted by {p} ({agree} agreeing neighbours)")
                    del known[c]
            known[x] = (p, "+".join(sorted({w.split()[0] for w in why})), "; ".join(why[:3]))
            used.add(p)
            added += 1
        if not added:
            return known, dmap, withdrawn


def consistency(pc, gcalls, gptrs, gcallers, twin, x, p):
    """(agreements, contradictions) of the pair x -> p with the pairs already known (twin: guest -> PC name).
      callee   a known guest callee whose twin the PC function never calls or references
      caller   a known guest caller whose twin never calls or references the PC function
    Inlining explains a missing call: MSVC /O2 inlines inline-sized functions (<= 32 bytes) and functions of the same
    source module; CodeWarrior (-inline auto) does the same, so a call may also land one level down, in a same-module
    callee. Contradictions are [(guest neighbour, text)]."""
    def inlinable(callee, caller):
        return pc[callee].size <= 32 or pc[callee].module == pc[caller].module

    def reach(y):
        r = set(pc[y].calls) | set(pc[y].fptrs)
        return r | {z for q in r if q in pc and pc[q].module == pc[y].module for z in pc[q].calls + pc[q].fptrs}
    agree, hard = 0, []
    mine = reach(p)
    for c in dict.fromkeys(gcalls.get(x, []) + gptrs.get(x, [])):
        if c in twin and c != x:
            if twin[c] in mine:
                agree += 1
            elif not inlinable(twin[c], p):
                hard.append((c, f"callee {twin[c]} not called by PC {p}"))
    for c in gcallers.get(x, ()):
        if c in twin and c != x:
            if p in reach(twin[c]):
                agree += 1
            elif not inlinable(p, twin[c]):
                hard.append((c, f"caller {twin[c]} does not call PC {p}"))
    return agree, hard


def audit(pc, gcalls, gptrs, known):
    """Every final pair checked against all its known neighbours: contradictions (see consistency) and, as a soft
    note only, known callees called in another order (the compilers lay out loops and error paths differently).
    Returns {guest address: [problems]}."""
    twin = {x: p for x, (p, _, _) in known.items()}
    gcallers = collections.defaultdict(set)
    for f, cs in list(gcalls.items()) + list(gptrs.items()):
        for c in cs:
            gcallers[c].add(f)
    out = {}
    for x, p in twin.items():
        _, hard = consistency(pc, gcalls, gptrs, gcallers, twin, x, p)
        probs = [t for _, t in hard]
        pfirst = list(dict.fromkeys(pc[p].calls))
        seq = [t for t in dict.fromkeys(twin[c] for c in gcalls.get(x, []) if c in twin) if t in pfirst]
        if [t for t in pfirst if t in seq] != seq:
            probs.append(f"(soft) order: known callees {seq[:6]} in another order on PC")
        if probs:
            out[x] = probs
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("spine")
    ap.add_argument("out")
    ap.add_argument("--holdout", type=float, default=0.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-tables", action="store_true", help="leave out rule T1 (data tables)")
    args = ap.parse_args(argv)
    pc, pc_order = load_pc()
    info, gcalls, rwset, gstrings, gptrs, gdata, gconsts, gobjs = load_guest(args.project, args.spine)
    pobjs = load_pc_data() if not args.no_tables else None
    seeds = {}
    with open(SEEDS, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            a = int(row["GuestAddr"], 16)
            if row["Confidence"] == "STRONG" and row["ProposedSymbol"] in pc and a in info:
                seeds[a] = (row["ProposedSymbol"], "S0", f"RW37SDK anchor ({row['Evidence'][:60]})")
    for a, name in PROVEN.items():
        seeds[a] = (name, "S0", "behaviour: rwcore oracle test, bit-identical")
    # GameCube driver functions named by role (dolrecon.rwdriver): RenderWare API names that the PC libraries define
    # too (_rwDeviceGetHandle) are seeds; every other one is driver code, never given a PC name
    more_driver = set()
    drv_path = os.path.join(args.out, "rw_driver.json")
    if os.path.exists(drv_path):
        with open(drv_path, encoding="utf-8") as f:
            for a_hex, row in json.load(f)["functions"].items():
                a = int(a_hex, 16)
                if row["name"] in pc and a in info:
                    seeds[a] = (row["name"], "S0", f"driver table: {row['evidence'][:60]}")
                else:
                    more_driver.add(a)
    # guest (PowerPC, CodeWarrior) over PC (x86, MSVC release) size: the seeds' middle 80 % lie in 0.64..2.06
    lo, hi = 0.5, 2.5
    hidden = {}
    if args.holdout:
        rnd = random.Random(args.seed)
        keys = sorted(seeds)
        rnd.shuffle(keys)
        for a in keys[:int(len(keys) * args.holdout)]:
            hidden[a] = seeds.pop(a)
    known, dmap, withdrawn = run(pc, pc_order, info, (gcalls, rwset, gstrings, gptrs, gdata, gconsts), seeds, lo, hi,
                      gobjs if pobjs is not None else None, pobjs, more_driver=more_driver)
    os.makedirs(args.out, exist_ok=True)
    problems = audit(pc, gcalls, gptrs, known)
    report = {"pc_functions": len(pc), "guest_rw_functions": len(rwset), "seeds": len(seeds),
              "named": len(known), "globals_mapped": len(dmap),
              "by_rule": dict(collections.Counter(r for _, r, _ in known.values())),
              "withdrawn": {f"0x{a:08X}": v for a, v in sorted(withdrawn.items()) if a not in known},
              "audit": {"pairs_with_problems": len(problems),
                        "problems": {f"0x{a:08X} {known[a][0]} ({known[a][1]})": v[:4]
                                     for a, v in sorted(problems.items())}}}
    if hidden:
        right = sum(1 for a, (p, _, _) in hidden.items() if a in known and known[a][0] == p)
        wrong = [(hex(a), p, known[a][0], known[a][1]) for a, (p, _, _) in hidden.items()
                 if a in known and known[a][0] != p]
        # a hidden seed's name given to ANOTHER guest function is wrong too (missed until 2026-11-01)
        owner = {p: a for a, (p, _, _) in known.items()}
        wrong += [(hex(a), p, f"name given to 0x{owner[p]:08X}", known[owner[p]][1]) for a, (p, _, _) in hidden.items()
                  if p in owner and owner[p] != a]
        report["holdout"] = {"hidden": len(hidden), "recovered_right": right, "recovered_wrong": len(wrong),
                             "not_recovered": len(hidden) - right - len(wrong), "wrong": wrong}
    else:
        rows = {f"0x{a:08X}": {"name": p, "module": pc[p].module, "rule": r, "evidence": e,
                               "guest_size": int(info[a]["size"]), "pc_size": pc[p].size}
                for a, (p, r, e) in sorted(known.items())}
        with open(os.path.join(args.out, "rw_names.json"), "w", encoding="utf-8") as f:
            # globals by address: independent of the names the analysed project happened to carry
            syms = mmatch.Project(args.project).symbols
            gl = sorted((syms[g].address, coff.readable(pd)) for g, pd in dmap.items())
            json.dump({"report": report, "names": rows, "globals": {f"0x{a:08X}": n for a, n in gl}}, f, indent=1)
        mods = collections.Counter(pc[p].module for p, _, _ in known.values())
        report["modules"] = dict(mods.most_common(40))
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
