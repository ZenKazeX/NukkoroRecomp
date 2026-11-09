"""Milestone 1: exact round trip of a DOL through dtk's assembly (docs/DOLRECON.md section 8).

    python -B -m dolrecon.rebuild <project dir>        (run from source/tools)

The project dir holds main.dol, config.yml and dtk's split output in out/ (dolrecon.project creates it). Steps:

  plan      every assembly block's [start, end) and section, from the "# 0xSTART..0xEND" line dtk writes before each
            section block; checked against the layout manifest: blocks may not overlap, and every byte of a loaded
            DOL section is either covered by a block or recorded as a preserved fragment (explicit bytes, reported).
  assemble  powerpc-eabi-as -mgekko per unit, bounded parallel workers.
  link      GNU ld with a generated script that pins every unit's block at its original address (". = ADDR" before
            it: ld refuses to move backwards, so an overlap or an oversized block fails the link instead of shifting).
  package   header regenerated from the manifest fields (its padding is a preserved fragment); each DOL section =
            the linked bytes of its range + preserved fragments; gaps and trailing bytes as preserved fragments.
  verify    level 1: each block's linked bytes vs the original bytes at its file offset; level 2: the whole file.
            Mismatches report file offset, address, expected / actual bytes and the responsible unit.

Preserved fragments are original bytes kept as data because no assembly owns them. They make an exact rebuild
possible but are NOT disassembly: report.json counts them separately (coverage vs accuracy).
"""

from __future__ import annotations

import concurrent.futures as cf
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
from dataclasses import dataclass

from . import dol as dolmod
from . import linker_tables

TOOLS = {
    "dtk": r"H:\Tools\dolrecon\dtk-1.8.4\dtk.exe",
    "as": r"H:\Tools\dolrecon\binutils-2.42-2\bin\powerpc-eabi-as.exe",
    "ld": r"H:\Tools\dolrecon\binutils-2.42-2\bin\powerpc-eabi-ld.exe",
    "objcopy": r"H:\Tools\dolrecon\binutils-2.42-2\bin\powerpc-eabi-objcopy.exe",
}
AS_FLAGS = ["-mgekko", "--strip-local-absolute"]
BLOCK_RE = re.compile(r"^# 0x([0-9A-Fa-f]{8})\.\.0x([0-9A-Fa-f]{8}) \| size: 0x([0-9A-Fa-f]+)\s*$")
SECTION_RE = re.compile(r"^\.(?:section\s+([^\s,]+)|(text|data|rodata|bss|init))\b")
BSS_SECTIONS = {".bss", ".sbss", ".sbss2"}


@dataclass
class Block:
    unit: str
    section: str
    start: int
    end: int


def tool_versions() -> dict:
    out = {}
    for name, path in TOOLS.items():
        flag = "--version" if name != "dtk" else "-V"
        first = subprocess.run([path, flag], capture_output=True, text=True).stdout.splitlines()
        out[name] = {"path": path, "version": first[0] if first else "?",
                     "sha256": hashlib.sha256(open(path, "rb").read()).hexdigest()}
    return out


def plan(asm_dir: str) -> list[Block]:
    blocks = []
    for name in sorted(os.listdir(asm_dir)):
        if not name.endswith(".s"):
            continue
        unit = name[:-2]
        pending = None
        with open(os.path.join(asm_dir, name), encoding="utf-8") as f:
            for line in f:
                m = BLOCK_RE.match(line)
                if m:
                    pending = (int(m.group(1), 16), int(m.group(2), 16))
                    continue
                s = SECTION_RE.match(line)
                if s and pending is not None:
                    section = s.group(1) or "." + s.group(2)
                    blocks.append(Block(unit, section, pending[0], pending[1]))
                    pending = None
    blocks.sort(key=lambda b: b.start)
    for a, b in zip(blocks, blocks[1:]):
        if b.start < a.end:
            raise ValueError(f"blocks overlap: {a} and {b}")
    return blocks


# CodeWarrior's compiler-generated names start with '@' (@62_80514448, @stringBase0_..., @etb_...). GNU ld reads '@' in
# a symbol name as a version separator and fails with "multiple definition of `no symbol' (*IND*)". The build copy of
# the assembly renames every identifier that starts with '@' (dtk writes them with an address suffix, e.g. @62 in
# symbols.txt is @62_80514448 in the asm) to AT_PREFIX + name[1:], outside string literals (".string" bytes are game
# data and stay untouched) and except GNU as's own section-type keywords. Names never reach the bytes (relocations
# resolve to addresses); verification proves that. A rename that would hit an existing name stops the build.
AT_PREFIX = "__at_"
AT_TOKEN = re.compile(r"(?<![\w.$@\"])@[A-Za-z0-9_$.]+")
QUOTED_AT = re.compile(r"\"(@[A-Za-z0-9_$.]+)\"")  # dtk quotes some symbol references: lis r3, "@1_805610F8"@ha
DATA_STRING = re.compile(r"^\s*\.(string|ascii|asciz)\b")
AS_KEYWORDS = {"@progbits", "@nobits", "@note", "@init_array", "@fini_array", "@preinit_array"}
IDENT = re.compile(r"[A-Za-z_.$][A-Za-z0-9_.$]*")


def _outside_strings(line: str, fn) -> str:
    parts = line.split('"')
    for i in range(0, len(parts), 2):  # even parts are outside quotes (dtk's strings escape inner quotes as \x22)
        parts[i] = fn(parts[i])
    return '"'.join(parts)


def prepare(out_dir: str, asm_out: str, symbols_txt: str) -> dict:
    src_dir = os.path.join(out_dir, "asm")
    files = sorted(n for n in os.listdir(src_dir) if n.endswith(".s"))
    texts = {}
    existing = set()
    for name in files:
        with open(os.path.join(src_dir, name), encoding="utf-8") as f:
            texts[name] = f.read()
        for line in texts[name].splitlines():
            _outside_strings(line, lambda seg: existing.update(IDENT.findall(seg)) or seg)
    renamed = set()
    count = 0

    def sub(m):
        nonlocal count
        t = m.group(0)
        if t in AS_KEYWORDS:
            return t
        target = AT_PREFIX + t[1:]
        if target in existing:
            raise ValueError(f"rename {t} -> {target} would hit an existing name")
        renamed.add(t)
        count += 1
        return target

    os.makedirs(asm_out, exist_ok=True)
    hint_fixes = []

    def line_fix(line: str) -> str:
        if DATA_STRING.match(line):  # string bytes are game data
            return line
        line = fix_branch_hint(line, hint_fixes)
        line = QUOTED_AT.sub(lambda m: sub(re.match(r".*", m.group(1))), line)
        return _outside_strings(line, lambda seg: AT_TOKEN.sub(sub, seg))

    for name in files:
        text = "\n".join(line_fix(line) for line in texts[name].split("\n"))
        with open(os.path.join(asm_out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    return {"renamed_symbols": len(renamed), "replaced_tokens": count, "prefix": AT_PREFIX,
            "branch_hint_fixes": hint_fixes}


# dtk 1.8.4 can print the wrong static-prediction suffix: for "41 A1 FF E4" (bgt, y bit set, backward) it writes
# bgt+ where GNU as needs bgt- (old y-bit rule: backward branches default to taken, y=1 reverses that; GNU as picks y
# from the suffix and the branch direction). The suffix is recomputed from the ORIGINAL word in dtk's own comment and
# the direction from the comment address and the target label's address; every change is recorded.
HINT_LINE = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  ([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2}) ([0-9A-F]{2}) \*/"
                       r"\t(b[a-z]*?)([+-])(\s+.*?)(?:\.L_|fn_|lbl_)([0-9A-F]{8})\b")


def fix_branch_hint(line: str, fixes: list) -> str:
    m = HINT_LINE.match(line)
    if not m:
        return line
    if m.group(6) in ("bc", "bcl", "bca", "bcla"):  # explicit BO operand: its own y bit, already exact
        return line
    addr, target = int(m.group(1), 16), int(m.group(9), 16)
    word = int("".join(m.group(i) for i in range(2, 6)), 16)
    opcode = word >> 26
    if opcode == 16:            # bc: BO in bits 21-25 (y = BO & 1)
        y = (word >> 21) & 1
    elif opcode == 19:          # bclr / bcctr: no displacement, the suffix rule is direction-free
        return line
    else:
        return line
    backward = target < addr
    want = ("-" if y else "+") if backward else ("+" if y else "-")
    if want == m.group(7):
        return line
    fixes.append({"address": f"{addr:#010x}", "word": f"{word:08x}", "dtk": m.group(6) + m.group(7),
                  "fixed": m.group(6) + want})
    i = m.start(7)
    return line[:i] + want + line[i + 1:]


def assemble(asm_dir: str, include: str, obj_dir: str, units: list[str], jobs: int) -> list[str]:
    os.makedirs(obj_dir, exist_ok=True)

    def one(unit: str) -> str | None:
        r = subprocess.run([TOOLS["as"], *AS_FLAGS, "-I", include, "-o", os.path.join(obj_dir, unit + ".o"),
                            os.path.join(asm_dir, unit + ".s")], capture_output=True, text=True)
        return None if r.returncode == 0 else f"{unit}: {r.stderr.strip()[:400]}"

    with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
        return [e for e in pool.map(one, units) if e]


def ldscript(blocks: list[Block], obj_dir: str, entry: str) -> str:
    by_section: dict[str, list[Block]] = {}
    order: list[str] = []
    for b in blocks:
        if b.section not in by_section:
            by_section[b.section] = []
            order.append(b.section)
        by_section[b.section].append(b)
    lines = [f"ENTRY({entry})", "SECTIONS", "{"]
    for sec in order:
        bs = by_section[sec]
        kind = " (NOLOAD)" if sec in BSS_SECTIONS else ""
        lines.append(f"  \"{sec}\" 0x{bs[0].start:08X}{kind} :")
        lines.append("  {")
        for b in bs:
            path = os.path.join(obj_dir, b.unit + ".o").replace("\\", "/")
            lines.append(f"    . = 0x{b.start - bs[0].start:X};")
            lines.append(f"    \"{path}\"(\"{sec}\")")
            lines.append(f"    ASSERT(. == 0x{b.end - bs[0].start:X}, \"{b.unit}: {sec} does not end at 0x{b.end:08X}\");")
        lines.append("  }")
    lines += ["  /DISCARD/ : { *(.comment) *(.note*) *(.mwcats*) }", "}"]
    return "\n".join(lines) + "\n"


UNDEF_RE = re.compile(r"undefined reference to `([^']+)'")
R_LO, R_HI, R_HA = 4, 5, 6  # R_PPC_ADDR16_LO / _HI / _HA
ALWAYS_RECOVER = {"_SDA_BASE_", "_SDA2_BASE_"}  # ld would otherwise define them itself (wrongly for this layout)


def _relocs(obj_path: str) -> list[tuple[str, int, int, str]]:
    """(section, offset, type, symbol) for every RELA entry of a 32-bit big-endian relocatable ELF."""
    with open(obj_path, "rb") as f:
        elf = f.read()
    shoff, = struct.unpack_from(">I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", elf, 0x2E)
    hdrs = [struct.unpack_from(">IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    shstr = elf[hdrs[shstrndx][4]:hdrs[shstrndx][4] + hdrs[shstrndx][5]]

    def cstr(blob, at):
        return blob[at:blob.index(b"\0", at)].decode()

    out = []
    for h in hdrs:
        if h[1] != 4:  # SHT_RELA
            continue
        symtab = hdrs[h[6]]
        strtab = hdrs[symtab[6]]
        strs = elf[strtab[4]:strtab[4] + strtab[5]]
        target = cstr(shstr, hdrs[h[7]][0])
        for i in range(h[5] // 12):
            r_off, r_info, _ = struct.unpack_from(">IIi", elf, h[4] + i * 12)
            sym_name_off = struct.unpack_from(">I", elf, symtab[4] + (r_info >> 8) * 16)[0]
            out.append((target, r_off, r_info & 0xFF, cstr(strs, sym_name_off)))
    return out


def recover_symbols(names: set[str], blocks: list[Block], obj_dir: str, orig, symbols_txt: str) -> dict:
    """Absolute values for symbols the linker would define (stack, SDA bases, init tables), recovered from the
    original instruction operands at their @ha / @l relocation sites. Every site must agree; symbols.txt, where it
    states an address, must agree too. Disagreement stops the build."""
    declared = {}
    with open(symbols_txt, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^(\S+) = [^:]+:0x([0-9A-Fa-f]+);", line)
            if m:
                declared[m.group(1)] = int(m.group(2), 16)
    starts = {(b.unit, b.section): b.start for b in blocks}
    found: dict[str, dict] = {n: {"ha": set(), "hi": set(), "lo": set(), "sites": 0} for n in names}
    for unit, section in sorted(starts):
        path = os.path.join(obj_dir, unit + ".o")
        for sec, off, rtype, sym in _relocs(path):
            if sym not in found or sec != section or rtype not in (R_HA, R_HI, R_LO):
                continue
            word = orig.word(starts[(unit, section)] + off - 2)  # the relocation points at the immediate's half
            imm = word & 0xFFFF
            found[sym]["ha" if rtype == R_HA else "hi" if rtype == R_HI else "lo"].add(imm)
            found[sym]["sites"] += 1
    values = {}
    for n, f in found.items():
        if (f["ha"] or f["hi"]) and len(f["lo"]) == 1:
            lo = next(iter(f["lo"]))
            # @ha pairs with a sign-extending @l (addi / loads), @h with ori; every site must give one value.
            candidates = {((ha << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF for ha in f["ha"]}
            candidates |= {(hi << 16) | lo for hi in f["hi"]}
            if len(candidates) != 1:
                raise ValueError(f"{n}: sites disagree: {sorted(hex(c) for c in candidates)}")
            value = next(iter(candidates))
            if n in declared and declared[n] != value:
                raise ValueError(f"{n}: operands give {value:#x}, symbols.txt says {declared[n]:#x}")
            values[n] = {"value": value, "provenance": f"@ha/@l operands at {f['sites']} sites"
                         + (", matches symbols.txt" if n in declared else "")}
        elif n in declared and not f["ha"] and not f["hi"] and not f["lo"]:
            values[n] = {"value": declared[n], "provenance": "symbols.txt (no @ha/@l site)"}
        else:
            raise ValueError(f"{n}: cannot recover (ha {sorted(f['ha'])}, hi {sorted(f['hi'])}, lo {sorted(f['lo'])})")
    return values


def link(script_path: str, elf_path: str) -> subprocess.CompletedProcess:
    return subprocess.run([TOOLS["ld"], "-T", script_path, "-o", elf_path, "--no-check-sections"],
                          capture_output=True, text=True)


def linked_image(elf_path: str, work: str, sections: list[str]) -> dict[str, tuple[int, bytes]]:
    """Each loadable output section's (address, bytes), read from the linked ELF's program bytes."""
    out = {}
    with open(elf_path, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        raise ValueError("expected a 32-bit big-endian ELF")
    shoff, = struct.unpack_from(">I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", elf, 0x2E)
    hdrs = [struct.unpack_from(">IIIIIIIIII", elf, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    names = elf[strtab[4]:strtab[4] + strtab[5]]
    for h in hdrs:
        name = names[h[0]:names.index(b"\0", h[0])].decode()
        if name in sections and h[1] != 8:  # SHT_NOBITS has no bytes
            out[name] = (h[3], elf[h[4]:h[4] + h[5]])
    return out


def run(project: str, jobs: int = 16) -> dict:
    t0 = time.perf_counter()
    times = {}
    orig = dolmod.load_file(os.path.join(project, "main.dol"))
    manifest = orig.manifest({"as_flags": AS_FLAGS})
    out_dir = os.path.join(project, "out")
    build = os.path.join(project, "build")
    os.makedirs(build, exist_ok=True)

    blocks = plan(os.path.join(out_dir, "asm"))
    times["plan_s"] = time.perf_counter() - t0
    units = sorted({b.unit for b in blocks})
    t = time.perf_counter()
    asm_build = os.path.join(build, "asm")
    renames = prepare(out_dir, asm_build, os.path.join(project, "config", "symbols.txt"))
    times["prepare_s"] = time.perf_counter() - t
    t = time.perf_counter()
    errors = assemble(asm_build, os.path.join(out_dir, "include"), os.path.join(build, "obj"), units, jobs)
    times["assemble_s"] = time.perf_counter() - t
    if errors:
        return {"ok": False, "stage": "assemble", "errors": errors}

    script = os.path.join(build, "link.ld")
    with open(script, "w", newline="\n") as f:
        f.write(ldscript(blocks, os.path.join(build, "obj"), "__start"))
    elf = os.path.join(build, "main.elf")
    t = time.perf_counter()
    r = link(script, elf)
    undefined = set(UNDEF_RE.findall(r.stderr)) | ALWAYS_RECOVER
    recovered = recover_symbols(undefined, blocks, os.path.join(build, "obj"), orig,
                                os.path.join(project, "config", "symbols.txt"))
    with open(script, "w", newline="\n") as f:
        f.write("".join(f"{n} = 0x{v['value']:08X};\n" for n, v in sorted(recovered.items())))
        f.write(ldscript(blocks, os.path.join(build, "obj"), "__start"))
    r = link(script, elf)
    times["link_s"] = time.perf_counter() - t
    if r.returncode != 0:
        return {"ok": False, "stage": "link", "recovered_symbols": {k: hex(v["value"]) for k, v in recovered.items()},
                "errors": r.stderr.splitlines()[:40]}

    sections = sorted({b.section for b in blocks} - BSS_SECTIONS)
    image = linked_image(elf, build, sections)

    # ---- package: header + per DOL section (linked bytes, else preserved) + gaps / trailing ----------------------
    rebuilt = bytearray(len(orig.raw))
    owner = ["?"] * 0  # (kept small: ownership is recorded per range below)
    ranges = []        # (file_offset, size, kind, owner)
    header = bytearray(dolmod.HEADER_SIZE)
    offs, addrs, sizes = [0] * 18, [0] * 18, [0] * 18
    for s in orig.sections:
        offs[s.index], addrs[s.index], sizes[s.index] = s.file_offset, s.address, s.size
    struct.pack_into(">18I", header, dolmod.OFF_OFFSETS, *offs)
    struct.pack_into(">18I", header, dolmod.OFF_ADDRS, *addrs)
    struct.pack_into(">18I", header, dolmod.OFF_SIZES, *sizes)
    struct.pack_into(">III", header, dolmod.OFF_BSS_ADDR, orig.bss_address, orig.bss_size, orig.entry)
    header[dolmod.HEADER_USED_END:] = orig.raw[dolmod.HEADER_USED_END:dolmod.HEADER_SIZE]
    rebuilt[0:dolmod.HEADER_SIZE] = header
    ranges.append((0, dolmod.HEADER_USED_END, "generated", "header fields (manifest)"))
    ranges.append((dolmod.HEADER_USED_END, dolmod.HEADER_SIZE - dolmod.HEADER_USED_END, "preserved", "header padding"))

    linked_spans = []  # (address, bytes)
    for name, (addr, data) in image.items():
        linked_spans.append((addr, data))
    preserved = []
    for s in orig.sections:
        cursor = s.address
        for addr, data in sorted(linked_spans):
            lo, hi = max(addr, s.address), min(addr + len(data), s.address_end)
            if lo >= hi:
                continue
            if lo > cursor:
                preserved.append((s, cursor, lo))
            rebuilt[s.file_offset + (lo - s.address):s.file_offset + (hi - s.address)] = data[lo - addr:hi - addr]
            ranges.append((s.file_offset + (lo - s.address), hi - lo, "assembled", s.name))
            cursor = hi
        if cursor < s.address_end:
            preserved.append((s, cursor, s.address_end))
    # Linker-generated tables (linker_tables.py): generated from the layout where the model reproduces them.
    syms = {}
    with open(os.path.join(project, "config", "symbols.txt"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^(\S+) = ([^:]+):0x([0-9A-Fa-f]+);", line)
            if m:
                syms[m.group(1)] = (int(m.group(3), 16), m.group(2))
    gen = linker_tables.generate(orig, blocks, syms)
    gen_spans = sorted((a, a + len(b), b, n) for a, b, n, _ in gen["tables"])
    for s, lo, hi in preserved:
        pieces, cur = [], lo
        for ga, gb, blob, name in gen_spans:
            a, b = max(ga, cur), min(gb, hi)
            if a >= b:
                continue
            if a > cur:
                pieces.append(("preserved", cur, a, None))
            pieces.append(("generated", a, b, (blob[a - ga:b - ga], name)))
            cur = b
        if cur < hi:
            pieces.append(("preserved", cur, hi, None))
        for kind, a, b, extra in pieces:
            off = s.file_offset + (a - s.address)
            if kind == "generated":
                rebuilt[off:off + (b - a)] = extra[0]
                ranges.append((off, b - a, "generated", f"{extra[1]} (linker table, from layout)"))
            else:
                rebuilt[off:off + (b - a)] = orig.raw[off:off + (b - a)]
                ranges.append((off, b - a, "preserved", f"{s.name} 0x{a:08X}..0x{b:08X}"))
    for r_ in orig.layout():
        if r_.kind in ("gap", "trailing"):
            rebuilt[r_.file_offset:r_.file_offset + r_.size] = orig.raw[r_.file_offset:r_.file_offset + r_.size]
            ranges.append((r_.file_offset, r_.size, "preserved", r_.name))

    # Ownership: every byte exactly once.
    ranges.sort()
    pos = 0
    for off, size, kind, who in ranges:
        if off != pos:
            raise AssertionError(f"ownership {'gap' if off > pos else 'overlap'} at file offset {pos:#x} ({who})")
        pos = off + size
    if pos != len(orig.raw):
        raise AssertionError(f"ownership ends at {pos:#x}, file is {len(orig.raw):#x}")

    out_dol = os.path.join(build, "main.rebuilt.dol")
    with open(out_dol, "wb") as f:
        f.write(rebuilt)

    # ---- verify ------------------------------------------------------------------------------------------------
    block_mismatches = []
    for b in blocks:
        if b.section in BSS_SECTIONS:
            continue
        off = orig.file_offset_of(b.start)
        exp, act = orig.raw[off:off + (b.end - b.start)], rebuilt[off:off + (b.end - b.start)]
        if exp != act:
            i = next(i for i in range(len(exp)) if exp[i] != act[i])
            block_mismatches.append({"unit": b.unit, "section": b.section, "file_offset": f"{off + i:#x}",
                                     "address": f"{b.start + i:#010x}", "expected": exp[i:i + 8].hex(),
                                     "actual": act[i:i + 8].hex()})
    file_ok = bytes(rebuilt) == orig.raw
    first_diff = None
    if not file_ok:
        i = next(i for i in range(len(orig.raw)) if orig.raw[i] != rebuilt[i])
        first_diff = {"file_offset": f"{i:#x}", "expected": orig.raw[i:i + 8].hex(), "actual": rebuilt[i:i + 8].hex()}

    by_kind = {}
    for off, size, kind, who in ranges:
        by_kind[kind] = by_kind.get(kind, 0) + size
    report = {
        "schema": "dolrecon.rebuild/1",
        "input": manifest["input"],
        "rebuilt_sha256": hashlib.sha256(rebuilt).hexdigest(),
        "byte_exact": file_ok,
        "first_difference": first_diff,
        "block_mismatches": block_mismatches[:50],
        "block_mismatch_count": len(block_mismatches),
        "units": len(units),
        "renames": renames,
        "recovered_symbols": {k: {"value": f"{v['value']:#010x}", "provenance": v["provenance"]}
                              for k, v in recovered.items()},
        "blocks": len(blocks),
        "bytes_by_owner": by_kind,
        "assembled_fraction": by_kind.get("assembled", 0) / len(orig.raw),
        "preserved_fragments": [{"file_offset": f"{o:#x}", "size": s, "what": w} for o, s, k, w in ranges
                                if k == "preserved"],
        "preserved_nonzero_bytes": sum(sum(1 for x in orig.raw[o:o + s] if x) for o, s, k, w in ranges
                                       if k == "preserved"),
        "generated_parts": [w for o, s, k, w in ranges if k == "generated"],
        "linker_tables_rejected": gen["rejected"],
        "tools": tool_versions(),
        "as_flags": AS_FLAGS,
        "jobs": jobs,
        "times_s": {**times, "total_s": time.perf_counter() - t0},
    }
    with open(os.path.join(build, "report.json"), "w") as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(build, "layout.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    return report


if __name__ == "__main__":
    rep = run(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 16)
    print(json.dumps({k: v for k, v in rep.items() if k not in ("preserved_fragments", "tools", "block_mismatches")},
                     indent=1))
    if rep.get("block_mismatches"):
        print(json.dumps(rep["block_mismatches"][:5], indent=1))
    sys.exit(0 if rep.get("byte_exact") else 1)
