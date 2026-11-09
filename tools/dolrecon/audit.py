"""Analysis-accuracy audit of a reconstruction project (docs/DOLRECON.md "Accuracy").

    python -B -m dolrecon.audit <project dir>          (run from source/tools; after dolrecon.project / rebuild)

A byte-exact rebuild proves the bytes, not the analysis: a wrong function boundary, a missed pointer or a constant
mistaken for an address all reassemble to the same bytes at the original layout. This audit judges dtk's analysis
against evidence taken from the ORIGINAL bytes (its own branch decoder, not dtk's text) and the compiler's own tables:

  boundaries
    B1 extab      extabindex entries (start, size, extab) are written by the compiler for every function with C++
                  exception info: exact ground truth for those functions.
    B2 calls      every `bl` / `bla` target must be a function start (a call into the middle of a function is a missed
                  boundary or a second entry point).
    B3 branches   every non-linking branch must stay inside its function or go to another function's start (tail call);
                  a branch into the middle of another function means a shared block or a wrong boundary.
    B4 ends       a function's last instruction must transfer control unconditionally (b, blr, bctr, rfi, ...);
                  otherwise it falls through into the next function (boundary too early, or a no-return call).
    B5 coverage   text bytes owned by no function, by kind (zero padding / other).
  references
    R1 missed     aligned data words whose value lies inside a loaded section or BSS but have no relocation:
                  candidate missed pointers, ranked by what they point at (function start / symbol start / inside).
    R2 targets    every ADDR32 relocation into text must hit a function start or a jump-table / code-label target;
                  anything else is suspicious.
    R3 raw hi     `lis rX, imm` in text with an address-like immediate and no relocation (a missed @ha / @h).

Each finding carries the address and the evidence; nothing is "fixed" here (dolrecon.corrections does that, and the
rebuild must stay byte-exact afterwards).
"""

from __future__ import annotations

import bisect
import collections
import json
import os
import re
import struct
import sys

from . import dol as dolmod
from . import rebuild

SYM_RE = re.compile(r"^(\S+) = (\S+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
R_ADDR32 = 1
UNCOND_ENDS = {"b", "blr", "bctr", "rfi", "ba"}


def load_symbols(path: str) -> dict[str, list]:
    out = collections.defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = SYM_RE.match(line)
            if m:
                out[m.group(4)].append((int(m.group(3), 16), int(m.group(5) or "0", 16), m.group(1), m.group(2)))
    for v in out.values():
        v.sort()
    return out


def decode_flow(word: int, address: int) -> tuple[str, int | None, bool]:
    """(kind, target, link) for control-flow words; kind '' for everything else. Decoded from the bits here so the
    audit does not depend on dtk's disassembly text."""
    op = word >> 26
    if op == 18:  # b / ba / bl / bla
        li = word & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        aa, lk = (word >> 1) & 1, word & 1
        return "b", (li if aa else address + li) & 0xFFFFFFFF, bool(lk)
    if op == 16:  # bc
        bd = word & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        aa, lk = (word >> 1) & 1, word & 1
        bo = (word >> 21) & 0x1F
        always = (bo & 0x14) == 0x14
        return ("b" if always else "bc"), (bd if aa else address + bd) & 0xFFFFFFFF, bool(lk)
    if op == 19:
        xo = (word >> 1) & 0x3FF
        bo = (word >> 21) & 0x1F
        always = (bo & 0x14) == 0x14
        if xo == 16:
            return ("blr" if always else "bclr"), None, bool(word & 1)
        if xo == 528:
            return ("bctr" if always else "bcctr"), None, bool(word & 1)
        if xo == 50:
            return "rfi", None, False
    return "", None, False


def run(project: str) -> dict:
    image = dolmod.load_file(os.path.join(project, "main.dol"))
    syms = load_symbols(os.path.join(project, "config", "symbols.txt"))
    funcs = [(a, s, n) for a, s, n, _ in syms["function"]]
    starts = [a for a, _, _ in funcs]
    start_set = set(starts)
    size_of = {a: s for a, s, _ in funcs}
    name_of = {a: n for a, _, n in funcs}
    labels = {a for a, _, _, _ in syms["label"]}
    objects = [(a, s, n) for a, s, n, _ in syms["object"]]
    object_starts = {a for a, _, _ in objects}
    jumptables = [(a, s, n) for a, s, n in objects if n.startswith("jumptable_")]
    text = [s for s in image.sections if s.kind == "text"]

    def owner(addr: int):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < starts[i] + size_of[starts[i]]:
            return starts[i]
        return None

    def in_text(addr: int) -> bool:
        return any(s.address <= addr < s.address_end for s in text)

    # ---- jump-table targets (they legitimately point into function bodies) -------------------------------------
    jt_targets = set()
    for a, s, _ in jumptables:
        for i in range(0, s, 4):
            jt_targets.add(image.word(a + i))

    report: dict = {"project": project, "input_sha256": image.sha256, "functions": len(funcs)}

    # ---- B1 extab -----------------------------------------------------------------------------------------------
    extab_index = next((s for s in image.sections if s.address == 0x80007520 or False), None)
    ext = {"entries": 0, "exact": 0, "start_missing": [], "size_differs": []}
    for a, s, n, sec in syms["object"]:
        pass
    eti = [s for s in image.sections if s.kind == "data"]
    # dtk names the section; find it from the symbol table (@eti_ objects live in extabindex)
    eti_objs = [(a, s) for a, s, n, sec in syms["object"] if sec == "extabindex"]
    if eti_objs:
        lo = min(a for a, _ in eti_objs)
        hi = max(a + s for a, s in eti_objs)
        for at in range(lo, hi - 11, 12):
            f_start, f_size, f_extab = struct.unpack(">III", image.read(at, 12))
            if not in_text(f_start):
                continue
            ext["entries"] += 1
            if f_start not in start_set:
                ext["start_missing"].append({"start": f"{f_start:#010x}", "size": f_size,
                                             "inside": None if owner(f_start) is None else f"{owner(f_start):#010x}"})
            elif size_of[f_start] != f_size:
                ext["size_differs"].append({"start": f"{f_start:#010x}", "extab_size": f_size,
                                            "dtk_size": size_of[f_start]})
            else:
                ext["exact"] += 1
    report["B1_extab"] = {**ext, "start_missing": ext["start_missing"][:200],
                          "start_missing_count": len(ext["start_missing"]),
                          "size_differs_count": len(ext["size_differs"]), "size_differs": ext["size_differs"][:200]}

    # ---- B2-B4: walk every word of every function ------------------------------------------------------------
    labeled_entries = collections.Counter()       # calls to labelled second entry points (fine)
    calls_mid = collections.defaultdict(list)     # target -> call sites (calls into a function body)
    calls_outside = []                             # bl into no function at all
    cross = collections.defaultdict(list)          # non-link branch into another function's body
    fallthrough = []
    flow_counts = collections.Counter()
    for a, size, n in funcs:
        if size == 0:
            continue
        last_kind = ""
        for pc in range(a, a + size, 4):
            word = image.word(pc)
            kind, target, link = decode_flow(word, pc)
            if not kind:
                last_kind = ""
                continue
            flow_counts[kind + ("l" if link else "")] += 1
            last_kind = kind if not link else kind + "l"
            if target is None:
                continue
            if link:
                if target in start_set:
                    continue
                if target in labels:  # a named second entry point (e.g. _savefpr_25 inside _savefpr_14)
                    labeled_entries[target] += 1
                    continue
                o = owner(target)
                if o is None:
                    calls_outside.append({"site": f"{pc:#010x}", "target": f"{target:#010x}"})
                else:
                    calls_mid[target].append(pc)
            else:
                if a <= target < a + size or target in start_set:
                    continue
                o = owner(target)
                cross[target].append((pc, a))
        if last_kind not in UNCOND_ENDS:
            nxt = a + size
            fallthrough.append({"function": f"{a:#010x}", "name": n, "last": f"{a + size - 4:#010x}",
                                "last_kind": last_kind or "other", "next_is_function": nxt in start_set})
    report["flow_counts"] = dict(flow_counts)
    report["B2_labeled_entry_calls"] = {"targets": len(labeled_entries), "sites": sum(labeled_entries.values())}
    report["B2_calls_into_bodies"] = {
        "targets": len(calls_mid), "sites": sum(len(v) for v in calls_mid.values()),
        "examples": [{"target": f"{t:#010x}", "inside": f"{owner(t):#010x}", "sites": [f"{s:#010x}" for s in v[:4]]}
                     for t, v in sorted(calls_mid.items())[:100]],
        "calls_to_no_function": calls_outside[:50], "calls_to_no_function_count": len(calls_outside)}
    report["B3_branches_into_other_bodies"] = {
        "targets": len(cross), "sites": sum(len(v) for v in cross.values()),
        "examples": [{"target": f"{t:#010x}", "inside": None if owner(t) is None else f"{owner(t):#010x}",
                      "from": [f"{s:#010x} (fn {f:#010x})" for s, f in v[:3]]} for t, v in sorted(cross.items())[:100]]}
    ft_kinds = collections.Counter(f["last_kind"] for f in fallthrough)
    report["B4_no_unconditional_end"] = {"count": len(fallthrough), "by_last_kind": dict(ft_kinds),
                                         "examples": fallthrough[:100]}

    # ---- B6 vtable slots must be function entry points ---------------------------------------------------------
    from . import corrections as corrmod
    sdata_sec = None
    for a, s_, n, sec in syms["object"]:
        if sec == ".sdata":
            sdata_sec = image.section_at_address(a)
            break
    b6 = {"classes": 0, "slots": 0, "not_function_start": 0, "examples": []}
    if sdata_sec is not None:
        table = corrmod.rtti_classes(image, (sdata_sec.address, sdata_sec.address_end), object_starts)
        b6["classes"] = len(table)
        for cls, info in table.items():
            for off, methods in [(0, info["methods"])] + [(s["offset"], s["methods"]) for s in info["secondary"]]:
                for i, f in enumerate(methods):
                    if f is None:
                        continue        # pure virtual
                    b6["slots"] += 1
                    if f not in start_set:
                        b6["not_function_start"] += 1
                        if len(b6["examples"]) < 100:
                            b6["examples"].append({"class": cls, "table_offset": off, "slot": i, "target": f"{f:#010x}",
                                                   "inside": None if owner(f) is None else f"{owner(f):#010x}"})
    report["B6_vtable_slots"] = b6

    # ---- B5 coverage of text ---------------------------------------------------------------------------------
    unowned = {"zero_bytes": 0, "other_bytes": 0, "ranges": []}
    for s in text:
        pos = s.address
        idx = bisect.bisect_left(starts, s.address)
        while pos < s.address_end:
            nxt = starts[idx] if idx < len(starts) and starts[idx] < s.address_end else s.address_end
            if nxt > pos:
                blob = image.read(pos, nxt - pos)
                zeros = blob.count(0)
                unowned["zero_bytes"] += zeros
                unowned["other_bytes"] += len(blob) - zeros
                if len(blob) != zeros:
                    holder = max(((a, n) for a, _, n, _ in syms["label"] + syms["object"] if a <= pos),
                                 default=(None, None))
                    unowned["ranges"].append({"start": f"{pos:#010x}", "size": len(blob), "nonzero": len(blob) - zeros,
                                              "symbol_at_or_before": holder[1]})
            if idx < len(starts) and starts[idx] < s.address_end:
                pos = max(pos, starts[idx] + size_of[starts[idx]])
                idx += 1
            else:
                pos = s.address_end
    unowned["ranges"] = unowned["ranges"][:100]
    report["B5_text_not_in_functions"] = unowned

    # ---- references --------------------------------------------------------------------------------------------
    blocks = rebuild.plan(os.path.join(project, "out", "asm"))
    obj_dir = os.path.join(project, "build", "obj")
    block_start = {(b.unit, b.section): b.start for b in blocks}
    reloc_sites = {}
    hi_sites = set()
    for (unit, section), start in block_start.items():
        path = os.path.join(obj_dir, unit + ".o")
        if not os.path.exists(path):
            continue
        for sec, off, rtype, sym in rebuild._relocs(path):
            if sec != section:
                continue
            if rtype == R_ADDR32:
                reloc_sites[start + off] = sym
            elif rtype in (rebuild.R_HA, rebuild.R_HI):
                hi_sites.add(start + off - 2)

    loaded = [(s.address, s.address_end) for s in image.sections]
    if image.bss_size:
        loaded.append((image.bss_address, image.bss_address + image.bss_size))

    def mapped(v: int) -> bool:
        return any(lo <= v < hi for lo, hi in loaded)

    sym_starts = start_set | object_starts | labels

    # R1: data words. Words inside the linker-generated tables are generated from the layout and byte-checked by the
    # rebuild (linker_tables.py), not references dtk should have found.
    table_ranges = [(a, a + s_) for a, s_, n, _ in syms["object"]
                    if n in ("_rom_copy_info", "_bss_init_info", "_eti_init_info")]
    missed = collections.Counter()
    missed_examples = []
    for s in image.sections:
        if s.kind != "data":
            continue
        for at in range(s.address, s.address_end - 3, 4):
            if at in reloc_sites:
                continue
            if any(lo <= at < hi for lo, hi in table_ranges):
                missed["linker_table"] += 1 if image.word(at) >= 0x80000000 else 0
                continue
            v = image.word(at)
            if v < 0x80000000 or not mapped(v):
                continue
            cls = ("function_start" if v in start_set else "symbol_start" if v in sym_starts
                   else "text_inside" if in_text(v) else "data_inside")
            missed[cls] += 1
            if cls in ("function_start", "symbol_start") and len(missed_examples) < 200:
                missed_examples.append({"site": f"{at:#010x}", "section": s.name, "value": f"{v:#010x}", "class": cls})
    report["R1_unrelocated_address_words"] = {"by_class": dict(missed), "strong_examples": missed_examples}

    # R2: relocation targets into text. dtk's local labels (".L_XXXXXXXX:") are branch / jump targets it proved.
    code_labels = set()
    lab = re.compile(r"^\.L_([0-9A-F]{8}):", re.M)
    asm_dir = os.path.join(project, "out", "asm")
    for name in os.listdir(asm_dir):
        if name.endswith(".s"):
            with open(os.path.join(asm_dir, name), encoding="utf-8") as f:
                code_labels.update(int(h, 16) for h in lab.findall(f.read()))
    r2 = collections.Counter()
    r2_examples = []
    for site, sym in reloc_sites.items():
        v = image.word(site)
        if not in_text(v):
            r2["to_data"] += 1
            continue
        if v in start_set:
            r2["function_start"] += 1
        elif v in jt_targets:
            r2["jump_table_target"] += 1
        elif v in labels:
            r2["label"] += 1
        elif v in code_labels:
            r2["code_label"] += 1
        else:
            r2["text_inside_other"] += 1
            if len(r2_examples) < 100:
                r2_examples.append({"site": f"{site:#010x}", "value": f"{v:#010x}", "symbol": sym,
                                    "inside": None if owner(v) is None else f"{owner(v):#010x}"})
    report["R2_relocation_targets"] = {"by_class": dict(r2), "suspicious_examples": r2_examples}

    # R3: lis with address-like immediates and no relocation
    # R3: `lis rD, hi` without a relocation, paired with the next instruction that adds a low half to rD
    # (addi / ori / a D-form load or store based on rD). The FULL value decides: a symbol start is a missed reference.
    r3 = collections.Counter()
    r3_strong = []
    lo_ops = {14: "addi", 24: "ori", 32: "lwz", 34: "lbz", 40: "lhz", 42: "lha", 48: "lfs", 50: "lfd",
              36: "stw", 38: "stb", 44: "sth", 52: "stfs", 54: "stfd"}
    code_ranges = [(a, a + size, a) for a, size, _ in funcs] + [(s.address, s.address_end, None) for s in text]
    for s in text:
        for pc in range(s.address, s.address_end, 4):
            w = image.word(pc)
            if (w >> 26) != 15 or ((w >> 16) & 0x1F) != 0 or pc in hi_sites:
                continue
            rd, hi = (w >> 21) & 0x1F, w & 0xFFFF
            value = None
            for nxt in range(pc + 4, min(pc + 24, s.address_end), 4):
                w2 = image.word(nxt)
                op = w2 >> 26
                if op in lo_ops and ((w2 >> 16) & 0x1F) == rd:
                    lo = w2 & 0xFFFF
                    value = ((hi << 16) | lo) if op == 24 else ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo))
                    value &= 0xFFFFFFFF
                    break
                if ((w2 >> 21) & 0x1F) == rd and op not in (36, 38, 44, 52, 54):
                    break  # rD overwritten first
            if value is None:
                r3["no_pairing"] += 1
                continue
            if not mapped(value):
                r3["not_an_address"] += 1
            elif value in sym_starts:
                r3["symbol_start"] += 1
                r3_strong.append({"site": f"{pc:#010x}", "value": f"{value:#010x}", "rd": rd,
                                  "in_function": None if owner(pc) is None else f"{owner(pc):#010x}"})
            else:
                r3["mapped_inside"] += 1
    report["R3_raw_lis"] = {"by_class": dict(r3), "strong": r3_strong[:200], "strong_count": len(r3_strong)}

    os.makedirs(os.path.join(project, "build"), exist_ok=True)
    with open(os.path.join(project, "build", "audit.json"), "w") as f:
        json.dump(report, f, indent=1)
    return report


def summary(r: dict) -> str:
    e = r["B1_extab"]
    lines = [
        f"functions {r['functions']}",
        f"B1 extab: {e['entries']} entries, exact {e['exact']}, start missing {e['start_missing_count']}, "
        f"size differs {e['size_differs_count']}",
        f"B2 calls into function bodies: {r['B2_calls_into_bodies']['targets']} targets / "
        f"{r['B2_calls_into_bodies']['sites']} sites; calls to no function {r['B2_calls_into_bodies']['calls_to_no_function_count']}",
        f"B2 calls to labelled second entries (fine): {r['B2_labeled_entry_calls']['targets']} targets / "
        f"{r['B2_labeled_entry_calls']['sites']} sites",
        f"B6 vtable slots: {r['B6_vtable_slots']['classes']} classes, {r['B6_vtable_slots']['slots']} slots, "
        f"not a function start {r['B6_vtable_slots']['not_function_start']}",
        f"B3 branches into other bodies: {r['B3_branches_into_other_bodies']['targets']} targets / "
        f"{r['B3_branches_into_other_bodies']['sites']} sites",
        f"B4 no unconditional end: {r['B4_no_unconditional_end']['count']} {r['B4_no_unconditional_end']['by_last_kind']}",
        f"B5 text outside functions: zero {r['B5_text_not_in_functions']['zero_bytes']} B, other "
        f"{r['B5_text_not_in_functions']['other_bytes']} B",
        f"R1 unrelocated address-like data words: {r['R1_unrelocated_address_words']['by_class']}",
        f"R2 relocation targets in text: {r['R2_relocation_targets']['by_class']}",
        f"R3 raw lis pairs (full value): {r['R3_raw_lis']['by_class']}",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    rep = run(sys.argv[1])
    print(summary(rep))
