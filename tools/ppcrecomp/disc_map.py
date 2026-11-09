"""Disc identity and the canonical address map between the GameCube releases.

Every guest address this source tree knows (extract/*.txt, the runtime's
literals, the scaffold's layout:: constants, the docs) is a PAL GUPP8P address:
PAL is the CANONICAL address space. The other releases of the same game are
the same program linked with a few functions and data objects of other sizes,
so each is the canonical program at different addresses. This tool

  identify   names the disc a main.dol belongs to (extract/discs.json: game id,
             region, revision, SHA-1 and size of main.dol), refusing an unknown one;
  build      derives the address map canonical -> that disc from the two DOLs
             and extract/functions.csv, with the evidence for every segment;
  check      fails unless a checked-in map is exactly what build produces;
  lookup     maps canonical addresses (one per argument) and says how;
  rebase     writes a generator input (extract/*.txt, functions.csv) for that
             disc, listing every line it cannot carry.

The map is a list of segments `kind canonical_start canonical_end delta`:

  fixed    the OS low memory below the first section, laid out by the IPL/OS
           identically on every disc (identity).
  code     instructions of a function whose normalized body is identical in
           both DOLs (only address-carrying fields masked: branch displacements,
           a lis high half, and a low half applied to a lis register or r2/r13;
           every other constant is compared). Every relative branch of such a
           body is then checked to land on the mapped target, so a segment's
           delta is verified, not assumed. One link order produced both DOLs:
           targets must increase, so a copy of a body found out of order is
           not a match.
  data     data addresses formed by those identical bodies (lis/addi pairs,
           r2/r13 offsets): each pair (canonical, target) inside one data
           section or .bss of both DOLs is a proof point; points out of layout
           order are dropped; a segment spans a run of proof points with one
           delta. Data words naming a mapped function are checked as well.
  aligned  equal instructions inside a `changed` function (a body that
           differs): an address there exists in both, but the function's
           meaning may not be the same.

Anything else has NO address in the other disc that this tool can prove: the
rest of a changed function, the data between proof points of different deltas,
heap addresses above the image. A consumer must refuse such an address, never
guess one.

  disc_map.py identify --catalog extract/discs.json --dol <main.dol>
  disc_map.py build --catalog extract/discs.json --canonical <PAL main.dol>
                    --dol <other main.dol> --functions extract/functions.csv --out <map>
  disc_map.py check ... --check <map>
  disc_map.py lookup --map <map> 0x80051BBC ...
  disc_map.py rebase --catalog ... --canonical ... --dol ... --map <map>
                     --input extract/native-owners.txt ... --out-dir <dir> --dropped <tsv>

No byte of either DOL is written to the map: it holds addresses and counts only.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import difflib
import hashlib
import json
import struct
import sys
from pathlib import Path

DOL_HEADER = 0x100
TEXT_SECTIONS = 7


class Dol:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.raw = self.path.read_bytes()
        if len(self.raw) < DOL_HEADER:
            raise ValueError(f"{path}: not a DOL (only {len(self.raw)} bytes)")
        off = struct.unpack(">18I", self.raw[0:72])
        addr = struct.unpack(">18I", self.raw[72:144])
        size = struct.unpack(">18I", self.raw[144:216])
        self.bss, self.bss_size, self.entry = struct.unpack(">3I", self.raw[216:228])
        self.sections = [(i, addr[i], size[i], off[i]) for i in range(18) if size[i]]
        self.sha1 = hashlib.sha1(self.raw).hexdigest()

    def text(self):
        return [(a, s, o) for i, a, s, o in self.sections if i < TEXT_SECTIONS]

    def data(self):
        return [(a, s, o) for i, a, s, o in self.sections if i >= TEXT_SECTIONS]

    def words(self, address: int, size: int, offset: int) -> list[int]:
        return list(struct.unpack(f">{size // 4}I", self.raw[offset:offset + size]))

    def word_at(self, address: int):
        for _, a, s, o in self.sections:
            if a <= address < a + s:
                return struct.unpack(">I", self.raw[o + address - a:o + address - a + 4])[0]
        return None

    def norm_at(self, address: int):
        """The normalized instruction at a text address (normalize_words over its section), or None."""
        if not hasattr(self, "_norm"):
            self._norm = [(a, normalize_words(self.words(a, s, o))) for a, s, o in self.text()]
        for a, n in self._norm:
            if a <= address < a + 4 * len(n):
                return n[(address - a) // 4]
        return None

    def data_region(self, address: int):
        """The data section index holding an address, 18 for .bss (with .sbss), None outside the data image."""
        for i, a, s, _ in self.sections:
            if i >= TEXT_SECTIONS and a <= address < a + s:
                return i
        return 18 if self.bss <= address < self.bss + self.bss_size else None

    def is_text(self, address: int) -> bool:
        return any(a <= address < a + s for a, s, _ in self.text())

    def layout(self) -> dict:
        return {
            "entry": f"0x{self.entry:08X}",
            "bss": [f"0x{self.bss:08X}", self.bss_size],
            "sections": [[i, f"0x{a:08X}", s] for i, a, s, _ in self.sections],
        }

    def small_data_bases(self) -> dict[int, int]:
        """r2 and r13 as __start sets them (lis rN,hi then ori/addi rN,rN,lo)."""
        bases: dict[int, int] = {}
        hi: dict[int, int] = {}
        for k in range(160):
            w = self.word_at(self.entry + 4 * k)
            if w is None:
                break
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0 and rd in (2, 13):
                hi[rd] = imm << 16
            elif op == 24 and rd in hi and rd == ra:  # ori rA,rS,imm: rS is bits 21-25
                bases[rd] = hi[rd] | imm
            elif op == 14 and rd in hi and ra == rd:
                bases[rd] = (hi[rd] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
        return bases


# ---- identity ---------------------------------------------------------------

def load_catalog(path: Path) -> dict:
    catalog = json.loads(Path(path).read_text(encoding="utf-8"))
    if catalog.get("canonical") not in {d["game_id"] for d in catalog["discs"]}:
        raise ValueError(f"{path}: the canonical disc is not in the catalogue")
    return catalog


def identify(catalog: dict, dol: Dol) -> dict:
    for disc in catalog["discs"]:
        if disc["main_dol"]["sha1"] == dol.sha1:
            if disc["main_dol"]["size"] != len(dol.raw):
                raise ValueError(f"{dol.path}: SHA-1 of {disc['game_id']} but size {len(dol.raw)}")
            return disc
    known = ", ".join(f"{d['game_id']} ({d['region']} rev {d['revision']})" for d in catalog["discs"])
    raise LookupError(f"{dol.path}: unknown main.dol (SHA-1 {dol.sha1}, {len(dol.raw)} bytes); "
                      f"known releases: {known}")


def read_boot_id(boot_bin: Path) -> tuple[str, int]:
    raw = Path(boot_bin).read_bytes()[:8]
    return raw[:6].decode("ascii"), raw[7]


# ---- normalization ----------------------------------------------------------

DFORM_MEM = set(range(32, 56)) | {56, 57, 60, 61}  # lwz..stfdu, psq_l(u), psq_st(u)
SMALL_DATA = (2, 13)


def normalize_words(words) -> list[int]:
    """Each instruction with exactly its address-carrying field masked: a branch displacement, the high half of a
    lis, and the low half (addi, ori/oris, D-form displacement) applied to a register holding a lis high half or
    to r2/r13. Every other immediate - li constants, stack and structure offsets - is compared as it is, so a
    body whose constants differ is not identical. The scan runs straight through a section (both DOLs alike);
    the tracked high halves end at any write of their register, at a call (volatile ones) and at blr/bctr/b."""
    out = []
    hi: set[int] = set()
    for w in words:
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        lis_ = op == 15 and ra == 0
        if op == 18:
            n = w & 0xFC000003
        elif op == 16:
            n = w & 0xFFFF0003
        elif lis_:
            n = w & 0xFFFF0000
        elif op == 14 and (ra in SMALL_DATA or ra in hi):
            n = w & 0xFFFF0000
        elif op in (24, 25) and rd in hi:  # ori/oris rA,rS,imm: rS is bits 21-25
            n = w & 0xFFFF0000
        elif op in DFORM_MEM and (ra in SMALL_DATA or ra in hi):
            n = w & 0xFFFF0000
        else:
            n = w
        out.append(n)
        for r in written_gprs(w):
            hi.discard(r)
        if lis_:
            hi.add(rd)
        if (op == 18 or op == 19) and w & 1:
            hi.difference_update((0, *range(3, 13)))
        elif op == 18 or (op == 19 and ((w >> 1) & 0x3FF) in (16, 528)):
            hi.clear()
    return out


def simm(w: int) -> int:
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def branch_target(w: int, at: int):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return li if w & 2 else (at + li) & 0xFFFFFFFF
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return bd if w & 2 else (at + bd) & 0xFFFFFFFF
    return None


# ---- matching ---------------------------------------------------------------

def load_functions(path: Path, dol: Dol) -> list[tuple[int, int]]:
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            a, s = int(r["addr_hex"], 16), int(r["size"])
            if s >= 4 and dol.is_text(a):
                rows.append((a, s))
    rows = sorted(set(rows))
    out = []
    for k, (a, s) in enumerate(rows):  # functions.csv rows can overlap the next start: clip them to it
        if k + 1 < len(rows) and rows[k + 1][0] == a:
            continue
        if k + 1 < len(rows):
            s = min(s, rows[k + 1][0] - a)
        out.append((a, s))
    return out


class Section:
    def __init__(self, dol: Dol, address: int, size: int, offset: int):
        self.address = address
        self.raw = dol.words(address, size, offset)
        self.norm = normalize_words(self.raw)

    def end(self):
        return self.address + 4 * len(self.raw)


PREFIX = 6
WINDOW = 0x40000  # a relocated function must lie within this many bytes of its prediction


def match_code(canon: Dol, other: Dol, functions):
    """Pairs canonical functions with their counterparts: [(start, size, delta, kind)].

    kind: 'code' (normalized body identical), 'changed' (no identical body nearby;
    delta is the best alignment of its entry, or None)."""
    csecs = [Section(canon, a, s, o) for a, s, o in canon.text()]
    osecs = [Section(other, a, s, o) for a, s, o in other.text()]
    if len(csecs) != len(osecs):
        raise ValueError("the DOLs have different text section counts")
    results = []
    for cs, os_ in zip(csecs, osecs):
        index = collections.defaultdict(list)
        for i in range(len(os_.norm) - PREFIX + 1):
            index[tuple(os_.norm[i:i + PREFIX])].append(i)
        delta = os_.address - cs.address
        pending = []
        for a, s in functions:
            if not (cs.address <= a < cs.end()):
                continue
            i, n = (a - cs.address) // 4, s // 4
            body = cs.norm[i:i + n]
            j = (a + delta - os_.address) // 4
            if 0 <= j and os_.norm[j:j + n] == body:
                results.append([a, s, delta, "code"])
                continue
            good = [c for c in index.get(tuple(body[:PREFIX]), ()) if os_.norm[c:c + n] == body
                    and abs(c - j) * 4 <= WINDOW] if n >= PREFIX else []
            if good:
                c = min(good, key=lambda c: abs(c - j))
                delta = os_.address + 4 * c - a
                results.append([a, s, delta, "code"])
            else:
                results.append([a, s, None, "changed"])
        # A function whose identical body also appears elsewhere can be paired with the wrong copy. One link
        # order produced both DOLs, so the true pairs have increasing, non-overlapping targets: keep the longest
        # such chain and demote every other 'code' row.
        rows = [r for r in results if cs.address <= r[0] < cs.end()]
        keep = monotonic_chain([r for r in rows if r[3] == "code"])
        for r in rows:
            if r[3] == "code" and id(r) not in keep:
                r[2], r[3] = None, "changed"
        align_changed(rows, cs, os_)
        for r in rows:  # a demoted row whose whole body lies in one equal block is the identical body after all
            if r[3] == "changed" and r[2] is not None and r[1] >= 4 * PREFIX and any(
                    s0 <= r[0] and r[0] + r[1] <= s1 and d == r[2] for s0, s1, d in r[4]):
                r[3] = "code"
    return results


def monotonic_chain(code) -> set[int]:
    """ids of the longest chain of rows whose targets increase without overlapping (patience sorting)."""
    tails: list[int] = []      # tails[k] = index into code of the best chain end of length k + 1
    parent = [-1] * len(code)
    for i, r in enumerate(code):
        t = r[0] + r[2]
        lo, hi = 0, len(tails)
        while lo < hi:     # first chain whose end lies at or past t
            mid = (lo + hi) // 2
            e = code[tails[mid]]
            if e[0] + e[2] + e[1] <= t:
                lo = mid + 1
            else:
                hi = mid
        parent[i] = tails[lo - 1] if lo else -1
        if lo == len(tails):
            tails.append(i)
        elif t + r[1] <= code[tails[lo]][0] + code[tails[lo]][2] + code[tails[lo]][1]:
            tails[lo] = i
    keep, i = set(), tails[-1] if tails else -1
    while i >= 0:
        keep.add(id(code[i]))
        i = parent[i]
    return keep


def align_changed(rows, cs: Section, os_: Section):
    """Aligns each gap of changed functions between two identical neighbours word by word (difflib on the
    normalized words). A changed function's entry gets a delta only when its first word lies in an equal block."""
    k = 0
    while k < len(rows):
        if rows[k][3] != "changed":
            k += 1
            continue
        e = k
        while e < len(rows) and rows[e][3] == "changed":
            e += 1
        c0 = rows[k][0]
        c1 = rows[e][0] if e < len(rows) else cs.end()
        if k > 0:
            p = rows[k - 1]
            o0 = p[0] + p[1] + p[2]
        else:
            o0 = os_.address
        o1 = rows[e][0] + rows[e][2] if e < len(rows) else os_.end()
        a = cs.norm[(c0 - cs.address) // 4:(c1 - cs.address) // 4]
        b = os_.norm[(o0 - os_.address) // 4:(o1 - os_.address) // 4]
        equal = []
        if a and b:
            for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
                if tag == "equal":
                    equal.append((c0 + 4 * i1, c0 + 4 * i2, o0 + 4 * j1 - (c0 + 4 * i1)))
        for r in rows[k:e]:
            for s0, s1, d in equal:
                if s0 <= r[0] < s1:
                    r[2] = d
                    break
            r.append(equal)
        k = e


def place_by_callers(canon: Dol, other: Dol, rows) -> int:
    """A changed row too small to place by its body (a lone blr, a stub) is placed by the direct branches that
    identical bodies make to its entry: every such caller must send it to one address, and the normalized body
    there must equal the canonical one. Returns how many rows it placed."""
    entries = {r[0]: r for r in rows if r[3] == "changed"}
    seen: dict[int, set] = collections.defaultdict(set)
    for a, s, d, kind, *_ in rows:
        if kind != "code":
            continue
        for k in range(s // 4):
            at = a + 4 * k
            w = canon.word_at(at)
            tgt = branch_target(w, at)
            if tgt in entries and not (w & 2):
                seen[tgt].add(branch_target(other.word_at(at + d), at + d))
    placed = 0
    for entry, targets in seen.items():
        r = entries[entry]
        if len(targets) != 1:
            continue
        o = next(iter(targets))
        n = r[1] // 4
        if all(canon.norm_at(entry + 4 * k) == other.norm_at(o + 4 * k) for k in range(n)):
            r[2], r[3] = o - entry, "code"
            placed += 1
    return placed


def verify_branches(canon_secs, other: Dol, code_map) -> tuple[int, list]:
    """Every relative branch inside an identical body must land where the map sends its canonical target."""
    checked, bad = 0, []
    for cs in canon_secs:
        for a, s, d in code_map:
            if not (cs.address <= a < cs.end()):
                continue
            for k in range(s // 4):
                at = a + 4 * k
                w = cs.raw[(at - cs.address) // 4]
                t = branch_target(w, at)
                if t is None or (w & 2):
                    continue
                mapped = lookup_code(code_map, t)
                if mapped is None:
                    continue
                ow = other.word_at(at + d)
                if branch_target(ow, at + d) != mapped:
                    bad.append((at, t, mapped))
                checked += 1
    return checked, bad


_STARTS: dict[int, list[int]] = {}


def _starts(rows) -> list[int]:
    key = id(rows)
    if key not in _STARTS or len(_STARTS[key]) != len(rows):
        _STARTS[key] = [r[0] for r in rows]
    return _STARTS[key]


def lookup_code(code_map, address):
    starts = _starts(code_map)
    k = bisect.bisect_right(starts, address) - 1
    if k >= 0 and address < code_map[k][0] + code_map[k][1]:
        return address + code_map[k][2]
    return None


def data_pairs(canon: Dol, other: Dol, code_map):
    """Data addresses the identical bodies form: (canonical, other) proof points."""
    cb, ob = canon.small_data_bases(), other.small_data_bases()
    pairs = collections.Counter()
    for a, s, d in code_map:
        hi: dict[int, tuple[int, int]] = {}
        for k in range(s // 4):
            w, v = canon.word_at(a + 4 * k), other.word_at(a + d + 4 * k)
            op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if op == 15 and ra == 0:
                hi[rd] = ((w & 0xFFFF) << 16, (v & 0xFFFF) << 16)
                continue
            base = None
            if ra in (2, 13) and ra in cb and ra in ob and (op == 14 or op in DFORM_MEM):
                base = (cb[ra], ob[ra])
            elif ra in hi and ra != 0 and (op == 14 or op in DFORM_MEM):
                base = hi[ra]
            elif op == 24 and rd in hi:  # ori rA,rS,uimm with rS = bits 21-25
                pairs[(hi[rd][0] | (w & 0xFFFF), hi[rd][1] | (v & 0xFFFF))] += 1
            if base is not None:
                pc = (base[0] + simm(w)) & 0xFFFFFFFF
                po = (base[1] + simm(v)) & 0xFFFFFFFF
                if pc >= 0x80000000 and po >= 0x80000000:
                    pairs[(pc, po)] += 1
            for r in written_gprs(w):
                hi.pop(r, None)
            if op == 18 and w & 1 or op == 19 and w & 1:
                for r in (0, *range(3, 13)):  # a call clobbers the volatile registers
                    hi.pop(r, None)
            elif op == 19 and ((w >> 1) & 0x3FF) in (16, 528):  # blr / bctr leave the body's flow
                hi.clear()
    return pairs


INT_LOADS = {32, 33, 34, 35, 40, 41, 42, 43, 46}
UPDATE_FORMS = {33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55, 57, 61}


def written_gprs(w: int) -> tuple[int, ...]:
    """GPRs an instruction can write (over-approximated: losing a proof point is harmless, keeping a stale
    high half is not)."""
    op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    out = []
    if op in (7, 8, 12, 13, 14, 15) or op in INT_LOADS:
        out.append(rd)
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29) or op in UPDATE_FORMS:
        out.append(ra)
    if op == 31:
        out += [rd, ra]
    return tuple(out)


def data_segments(pairs, canon: Dol, other: Dol):
    """Runs of proof points with one delta inside one data section (or .bss) of both DOLs. A value outside the
    data image (a lis constant such as a byte mask) proves nothing; an address proven with two deltas is a
    conflict and proves nothing."""
    by_addr: dict[int, set] = collections.defaultdict(set)
    for (c, o) in pairs:
        region = canon.data_region(c)
        if region is not None and other.data_region(o) == region:
            by_addr[c].add((o - c, region))
    conflicts = [c for c, ds in sorted(by_addr.items()) if len(ds) > 1]
    points = [(c, *next(iter(ds)), pairs[(c, c + next(iter(ds))[0])])
              for c, ds in sorted(by_addr.items()) if len(ds) == 1]
    # One link order laid out both data images, so true points have non-decreasing targets. A constant that
    # merely falls inside the data range (lis r3,0x8052; addi r3,r3,1 forming 0x80520001 in both DOLs) breaks
    # that order: keep the chain with the most references and drop the rest.
    keep = heaviest_monotone_chain([(c + d, n) for c, d, _, n in points])
    rejected = len(points) - len(keep)
    segs = []
    last_region = None
    for k in sorted(keep):
        c, d, region, _ = points[k]
        if segs and segs[-1][2] == d and region == last_region:
            segs[-1][1] = c + 1
            segs[-1][3] += 1
        else:
            segs.append([c, c + 1, d, 1])
        last_region = region
    # A lone point between two runs of one delta (same section) contradicts the layout as well: no object between
    # two equally moved objects can move differently unless sizes change and change back. Drop it, join the runs.
    k = 1
    while k + 1 < len(segs):
        p, s, n = segs[k - 1], segs[k], segs[k + 1]
        same = canon.data_region(p[0]) == canon.data_region(n[0]) == canon.data_region(s[0])
        if same and s[3] == 1 and p[2] == n[2] != s[2]:
            p[1], p[3] = n[1], p[3] + n[3]
            del segs[k:k + 2]
            rejected += 1
        else:
            k += 1
    return segs, conflicts, rejected


def heaviest_monotone_chain(items) -> set[int]:
    """Indices of the subsequence of (target, weight) with non-decreasing targets and the largest total weight
    (a Fenwick tree over the target ranks; ties keep the earlier chain)."""
    ranks = {t: r for r, t in enumerate(sorted({t for t, _ in items}), 1)}
    size = len(ranks)
    tree = [(0, -1)] * (size + 1)  # (best weight, index of its last item) over target ranks
    parent = [-1] * len(items)
    best_total, best_end = 0, -1
    for i, (t, w) in enumerate(items):
        r, q = ranks[t], (0, -1)
        while r > 0:
            q = max(q, tree[r], key=lambda x: x[0])
            r -= r & -r
        total = q[0] + w
        parent[i] = q[1]
        r = ranks[t]
        while r <= size:
            if total > tree[r][0]:
                tree[r] = (total, i)
            r += r & -r
        if total > best_total:
            best_total, best_end = total, i
    keep, i = set(), best_end
    while i >= 0:
        keep.add(i)
        i = parent[i]
    return keep


def verify_pointer_words(canon: Dol, other: Dol, code_map, segs) -> tuple[int, int]:
    """Data words that name a function start: the mapped word must name the mapped function."""
    starts = {a for a, _, _ in code_map}
    ok = bad = 0
    for a, s, o in canon.data():
        for k, w in enumerate(canon.words(a, s, o)):
            if w not in starts:
                continue
            at = a + 4 * k
            m = lookup_data(segs, at)
            if m is None:
                continue
            if other.word_at(m) == lookup_code(code_map, w):
                ok += 1
            else:
                bad += 1
    return ok, bad


def lookup_data(segs, address):
    starts = _starts(segs)
    k = bisect.bisect_right(starts, address) - 1
    if k >= 0 and address < segs[k][1]:
        return address + segs[k][2]
    return None


# ---- the map ----------------------------------------------------------------

def build(catalog: dict, canon: Dol, other: Dol, functions_csv: Path) -> str:
    cdisc, odisc = identify(catalog, canon), identify(catalog, other)
    if cdisc["game_id"] != catalog["canonical"]:
        raise ValueError(f"{canon.path} is {cdisc['game_id']}, not the canonical {catalog['canonical']}")
    functions = load_functions(functions_csv, canon)
    rows = match_code(canon, other, functions)
    while place_by_callers(canon, other, rows):
        pass
    code_rows = [(a, s, d) for a, s, d, kind, *_ in rows if kind == "code"]
    # merge adjacent functions of one delta into segments (padding between them is part of neither)
    code_segs = []
    previous = None
    for a, s, d, kind, *_ in rows:
        if kind == "code":
            if previous == "code" and code_segs[-1][2] == d:
                code_segs[-1][1] = a + s  # the alignment padding between two identical neighbours moves with them
            else:
                code_segs.append([a, a + s, d])
        previous = kind
    canon_secs = [Section(canon, a, s, o) for a, s, o in canon.text()]
    checked, bad = verify_branches(canon_secs, other, code_rows)
    if bad:
        raise ValueError(f"{len(bad)} of {checked} branches of identical bodies disagree with the code map, "
                         f"first 0x{bad[0][0]:08X}")
    pairs = data_pairs(canon, other, code_rows)
    dsegs, conflicts, off_order = data_segments(pairs, canon, other)
    ptr_ok, ptr_bad = verify_pointer_words(canon, other, code_rows, dsegs)
    changed = [r for r in rows if r[3] == "changed"]
    lines = [
        f"# Address map {catalog['canonical']} (canonical) -> {odisc['game_id']} ({odisc['region']}, "
        f"rev {odisc['revision']}).",
        "# Generated by tools/ppcrecomp/disc_map.py build; do not edit (disc_map.py check refuses a stale map).",
        f"# canonical main.dol sha1 {canon.sha1}",
        f"# target    main.dol sha1 {other.sha1}",
        f"# functions {len(rows)}: identical {len(code_rows)}, changed {len(changed)}",
        f"# branches verified {checked}; data proof points {len(pairs)} (conflicting addresses "
        f"{len(conflicts)}, out of layout order {off_order}); pointer words verified {ptr_ok}, disagreeing {ptr_bad}",
        "# segment: fixed|code|data canonical_start canonical_end delta [proof_points]",
        "# changed: canonical_start size entry_delta|- equal_words/size_words (a body that differs)",
        "# aligned: canonical_start canonical_end delta (equal instructions inside a changed body)",
    ]
    # The OS low memory below the first section (globals the IPL and the OS lay out at fixed addresses) is the
    # same memory on every disc whose image starts at the same address.
    low = min(a for _, a, _, _ in canon.sections)
    if low == min(a for _, a, _, _ in other.sections) and low > 0x80000000:
        lines.append(f"fixed 0x80000000 0x{low:08X} +0x0")
    for a, e, d in code_segs:
        lines.append(f"code 0x{a:08X} 0x{e:08X} {d:+#x}")
    for a, e, d, n in dsegs:
        lines.append(f"data 0x{a:08X} 0x{e:08X} {d:+#x} {n}")
    aligned = []
    for r in changed:
        a, s, d = r[0], r[1], r[2]
        blocks = [(max(s0, a), min(s1, a + s), bd) for s0, s1, bd in (r[4] if len(r) > 4 else ())
                  if s1 > a and s0 < a + s]
        eq = sum(e - b for b, e, _ in blocks) // 4
        lines.append(f"changed 0x{a:08X} {s} {('%+#x' % d) if d is not None else '-'} {eq}/{s // 4}")
        aligned += blocks
    for a, e, d in aligned:
        lines.append(f"aligned 0x{a:08X} 0x{e:08X} {d:+#x}")
    for c in conflicts:
        lines.append(f"conflict 0x{c:08X}")
    return "\n".join(lines) + "\n"


class AddressMap:
    def __init__(self, text: str):
        self.fixed, self.code, self.data, self.aligned, self.changed = [], [], [], [], {}
        for line in text.splitlines():
            if not line or line.startswith("#"):
                continue
            f = line.split()
            if f[0] in ("fixed", "code", "data", "aligned"):
                getattr(self, f[0]).append((int(f[1], 16), int(f[2], 16), int(f[3], 16)))
            elif f[0] == "changed":
                self.changed[int(f[1], 16)] = int(f[2])
        self.starts = {id(s): [x[0] for x in s] for s in (self.fixed, self.code, self.data, self.aligned)}

    def map(self, address: int):
        """(target, how) or (None, why)."""
        for segs, kind in ((self.fixed, "fixed"), (self.code, "code"), (self.data, "data"),
                           (self.aligned, "aligned")):
            k = bisect.bisect_right(self.starts[id(segs)], address) - 1
            if k >= 0 and address < segs[k][1]:
                return address + segs[k][2], kind
        for a, s in self.changed.items():
            if a <= address < a + s:
                return None, f"inside changed function 0x{a:08X}"
        return None, "no proof"


# ---- rebasing canonical inputs ----------------------------------------------
#
# Each generator input names canonical addresses in fixed columns; the rest of a line is a note, kept verbatim (it
# documents the canonical program). A column holding an instruction word pinned at a site is re-read from the
# target DOL at the mapped site and must equal the canonical word up to its address fields; a relative branch must
# also reach the mapped target. A line any of whose parsed addresses cannot be proven is not carried: it is
# reported, never guessed.

# name: (address columns, (site column, word column) or None); columns count whitespace fields before any '#'
SCHEMAS = {
    "native-owners": ((0,), None),
    "empty-variadic-leaves": ((0,), None),
    "wait-points": ((0, 1), None),
    "jump-tables": ((0, 1), None),
    "psq-sites": ((0,), None),
    "gx-sites": ((0,), (0, 1)),
    "platform-sites": ((0,), (0, 1)),
}
# lists whose rows stand for a whole function body (a native owner serves it, a leaf template replaces it)
WHOLE_BODY = {"native-owners", "empty-variadic-leaves"}


class Rebaser:
    def __init__(self, amap: AddressMap, canon: Dol, other: Dol):
        self.map, self.canon, self.other = amap, canon, other
        self.dropped: list[tuple[str, str, str]] = []

    def address(self, text: str):
        t, how = self.map.map(int(text, 16))
        return (None, how) if t is None else (f"0x{t:08X}", how)

    def word(self, site_text: str, word_text: str):
        site, word = int(site_text, 16), int(word_text, 16)
        target, how = self.map.map(site)
        if target is None:
            return None, how
        held = self.canon.word_at(site)
        if held != word:
            return None, f"stale: the canonical DOL holds 0x{held or 0:08X} at the site"
        tw = self.other.word_at(target)
        if tw is None or self.other.norm_at(target) != self.canon.norm_at(site):
            return None, f"the target instruction differs (0x{tw or 0:08X})"
        b = branch_target(word, site)
        if b is not None and not (word & 2):
            mb, _ = self.map.map(b)
            if mb != branch_target(tw, target):
                return None, f"the branch target 0x{b:08X} does not map to the target's"
        return f"0x{tw:08X}", how

    def columns(self, fields, addr_cols, pinned, whole_body=False):
        out = list(fields)
        for c in addr_cols:
            v, how = self.address(fields[c])
            if v is None:
                return None, f"0x{int(fields[c], 16):08X}: {how}"
            if whole_body and how != "code":  # it replaces the body, so the body must be the identical one
                return None, f"0x{int(fields[c], 16):08X}: the function body differs ({how})"
            out[c] = v
        if pinned:
            s, w = pinned
            v, how = self.word(fields[s], fields[w])
            if v is None:
                return None, f"0x{int(fields[s], 16):08X}: {how}"
            out[w] = v
        return out, None

    @staticmethod
    def parsed_count(name: str, fields) -> int:
        if name == "liveness-roots":
            return len(fields)
        if name == "lift-sites":
            return 3 if fields[0] == "retire" else 4
        cols, pinned = SCHEMAS[name]
        return max(max(cols), pinned[1] if pinned else 0) + 1

    def rebase_fields(self, name: str, fields):
        if name == "liveness-roots":  # root [census site...]
            out = []
            for f in fields:
                if f == "census":
                    out.append(f)
                    continue
                v, how = self.address(f)
                if v is None:
                    if out:  # an unprovable constructing site only drops that evidence, not the root
                        continue
                    return None, f"0x{int(f, 16):08X}: {how}"
                out.append(v)
            return out, None
        if name == "lift-sites":
            if fields[0] == "retire":
                return self.columns(fields, (1, 2), None)
            return self.columns(fields, (1,), (1, 2))
        cols, pinned = SCHEMAS[name]
        return self.columns(fields, cols, pinned, whole_body=name in WHOLE_BODY)

    def rebase_lines(self, name: str, text: str) -> str:
        out = []
        for n, line in enumerate(text.splitlines(), 1):
            if name != "lift-sites" and line.lstrip().startswith("#"):
                out.append(line)
                continue
            body, hash_, note = line.partition("#") if name in ("lift-sites", "liveness-roots") else (line, "", "")
            fields = body.split()
            if not fields:
                out.append(line)
                continue
            parsed = self.parsed_count(name, fields)
            new, why = self.rebase_fields(name, fields[:parsed])
            if new is None:
                self.dropped.append((name, f"line {n}", why))
                continue
            rest = body.split(None, parsed)[parsed] if len(fields) > parsed else ""
            out.append(" ".join(new) + ((" " + rest) if rest else "") + (hash_ + note if hash_ else ""))
        return "\n".join(out) + "\n"

    def rebase_functions(self, text: str) -> str:
        rows = list(csv.reader(text.splitlines()))
        header, body = rows[0], rows[1:]
        carried = []
        for r in body:
            a = int(r[0], 16)
            t, how = self.map.map(a)
            if t is None or how not in ("code", "aligned"):
                self.dropped.append(("functions", f"0x{a:08X}", how))
                continue
            callees = []
            for c in r[7].split():
                ct, _ = self.map.map(int(c, 16))
                if ct is not None:
                    callees.append(f"0x{ct:08x}")
            name = f"fn_{t:08x}" if r[2] == f"fn_{a:08x}" else r[2]
            carried.append([f"0x{t:08x}", str(t), name, r[3], r[4], r[5], r[6], " ".join(callees), how])
        carried.sort(key=lambda r: int(r[1]))
        def quoted(s: str) -> str:
            return '"' + s.replace('"', '""') + '"'
        out = [",".join(header)]
        for k, r in enumerate(carried):
            if r[8] == "aligned" and k + 1 < len(carried):  # a changed body ends at the next row at the latest
                r[3] = str(min(int(r[3]), int(carried[k + 1][1]) - int(r[1])))
            a = int(r[1])  # and every row ends inside the target's text section
            end = next((x + s for x, s, _ in self.other.text() if x <= a < x + s), a)
            r[3] = str(min(int(r[3]), end - a))
            # functions.csv's own form: numbers bare, text quoted
            out.append(",".join([r[0], r[1], quoted(r[2]), r[3], r[4], quoted(r[5]), quoted(r[6]), quoted(r[7])]))
        return "\n".join(out) + "\n"


def rebase_kind(path: Path) -> str:
    stem = path.stem
    if stem.startswith("gx-fail-sites"):
        return "gx-sites"
    for key in ("functions", "liveness-roots", "lift-sites", *SCHEMAS):
        if stem == key or stem.startswith(key + "-"):
            return key
    raise ValueError(f"{path}: no rebase schema for this file")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=("identify", "build", "check", "lookup", "rebase"))
    p.add_argument("--input", type=Path, action="append", default=[],
                   help="rebase: a canonical generator input (repeatable), written to --out-dir under its own name")
    p.add_argument("--out-dir", type=Path)
    p.add_argument("--dropped", type=Path, help="rebase: where to list the lines that could not be carried")
    p.add_argument("--catalog", type=Path)
    p.add_argument("--dol", type=Path)
    p.add_argument("--canonical", type=Path)
    p.add_argument("--functions", type=Path)
    p.add_argument("--out", type=Path)
    p.add_argument("--check", type=Path)
    p.add_argument("--map", type=Path)
    p.add_argument("--field", help="identify: print only this catalogue field (e.g. game_id)")
    p.add_argument("addresses", nargs="*")
    a = p.parse_args(argv)
    try:
        if a.command == "identify":
            disc = identify(load_catalog(a.catalog), Dol(a.dol))
            print(disc[a.field] if a.field else json.dumps(disc, indent=2))
            return 0
        if a.command in ("build", "check"):
            text = build(load_catalog(a.catalog), Dol(a.canonical), Dol(a.dol), a.functions)
            if a.command == "build":
                if a.out:
                    if a.out.exists():
                        a.out.unlink()
                    a.out.write_bytes(text.encode("utf-8"))
                else:
                    sys.stdout.write(text)
                return 0
            if a.check.read_bytes().decode("utf-8") != text:
                print(f"{a.check}: stale; regenerate with disc_map.py build", file=sys.stderr)
                return 1
            return 0
        m = AddressMap(a.map.read_text(encoding="utf-8"))
        if a.command == "rebase":
            catalog = load_catalog(a.catalog)
            canon, other = Dol(a.canonical), Dol(a.dol)
            if identify(catalog, canon)["game_id"] != catalog["canonical"]:
                raise ValueError(f"{a.canonical} is not the canonical disc")
            identify(catalog, other)
            rb = Rebaser(m, canon, other)
            a.out_dir.mkdir(parents=True, exist_ok=True)
            for src in a.input:
                text = src.read_bytes().decode("utf-8")
                kind = rebase_kind(src)
                new = rb.rebase_functions(text) if kind == "functions" else rb.rebase_lines(kind, text)
                (a.out_dir / src.name).write_bytes(new.encode("utf-8"))
            if a.dropped:
                report = "".join(f"{name}\t{where}\t{why}\n" for name, where, why in rb.dropped)
                a.dropped.write_bytes(report.encode("utf-8"))
            counts = collections.Counter(name for name, _, _ in rb.dropped)
            print(f"rebased {len(a.input)} file(s); lines not carried: {dict(counts) if counts else 0}")
            return 0
        status = 0
        for s in a.addresses:
            t, how = m.map(int(s, 16))
            print(f"{int(s, 16):#010x} -> " + (f"{t:#010x} ({how})" if t is not None else f"UNMAPPED ({how})"))
            status |= t is None
        return status
    except (LookupError, ValueError) as e:
        print(f"disc_map: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
