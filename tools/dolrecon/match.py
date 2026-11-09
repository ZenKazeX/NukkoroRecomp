"""Matching decompilation: compile candidate source with Metrowerks CodeWarrior and compare it to the original code.

    python -B -m dolrecon.match <project> <source.c|.cpp> [--fn NAME ...] [--cc 2.6] [--flags "..."] [--sweep]
                                [--json out.json] [--objdiff]

A function is MATCHED only when every rule below holds; anything less is reported with a fuzzy score and the first
differing instructions, never rounded up:
  M1  the compiled symbol has the same size as the original function;
  M2  every 32-bit word is equal outside the bits that a relocation rewrites;
  M3  relocations sit at the same offsets with the same types and addends;
  M4  each relocation names the same symbol as the original (CodeWarrior '@' names compare as the split's '__at_'),
      or, when the candidate points at its own data (a literal or a static in the same object), the bytes it points
      at equal the original bytes at the original target, in a section of the same kind.
The original side is the GNU as object assembled from the dtk split (build/obj/<unit>.o): its bytes are the DOL's
bytes (the rebuild is byte-exact) and its relocations are dtk's. Original data for M4 is read from main.dol.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import itertools
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

from . import elfobj

COMPILER_ROOT = r"H:\Tools\dolrecon\compilers-20251118\GC"
OBJDIFF = r"H:\Tools\dolrecon\objdiff-3.6.1\objdiff-cli.exe"
OBJCOPY = r"H:\Tools\dolrecon\binutils-2.42-2\bin\powerpc-eabi-objcopy.exe"
VERSIONS = ["1.0", "1.1", "1.1p1", "1.2.5", "1.2.5n", "1.3", "1.3.2", "1.3.2r", "2.0", "2.0p1", "2.5", "2.6", "2.7",
            "3.0a3", "3.0a3p1", "3.0a3.2", "3.0a3.3", "3.0a3.4", "3.0a5", "3.0a5.2"]
BASE_FLAGS = "-nodefaults -proc gekko -align powerpc -enum int -fp hardware -nosyspath"
# Flag sets the sweep tries on top of BASE_FLAGS. Each is a real compiler configuration; none edits output.
FLAG_SETS = {
    "O4p": "-O4,p -inline auto",
    "O4s": "-O4,s -inline auto",
    "O4p-noinline": "-O4,p -inline off",
    "O3": "-O3 -inline auto",
    "O2": "-O2 -inline auto",
    "O1": "-O1",
    "O0": "-O0",
    "O4p-contract": "-O4,p -inline auto -fp_contract on",
    "O4p-schedoff": "-O4,p -inline auto -schedule off",
}
DATA_CLASS = {".sdata": "sdata", ".sbss": "sdata", ".sdata2": "sdata2", ".sbss2": "sdata2", ".data": "data",
              ".bss": "data", ".rodata": "rodata"}


def _data_class(section: str) -> str:
    return DATA_CLASS.get(section, section)


# ---------------------------------------------------------------------------------------------------------------
# Original side
# ---------------------------------------------------------------------------------------------------------------

@dataclass
class TargetSymbol:
    name: str
    section: str
    address: int
    size: int
    kind: str


class Project:
    def __init__(self, root: str):
        self.root = root
        self.symbols: dict[str, TargetSymbol] = {}
        self.by_address: dict[int, TargetSymbol] = {}
        sym_re = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
        with open(os.path.join(root, "config", "symbols.txt"), encoding="utf-8") as f:
            for line in f:
                m = sym_re.match(line)
                if not m:
                    continue
                s = TargetSymbol(m[1], m[2], int(m[3], 16), int(m[5] or "0", 16), m[4])
                self.symbols[s.name] = s
                self.by_address.setdefault(s.address, s)
        names = os.path.join(root, "names.json")
        self.renames: dict[str, str] = {}
        if os.path.exists(names):
            with open(names, encoding="utf-8") as f:
                for row in json.load(f)["names"].values():
                    self.renames[row["old"]] = row["new"]
        with open(os.path.join(root, "build", "layout.json"), encoding="utf-8") as f:
            layout = json.load(f)
        self.dol_sections = [(int(s["address"], 16), s["size"], int(s["file_offset"], 16), s["name"])
                             for s in layout["sections"]]
        with open(os.path.join(root, "main.dol"), "rb") as f:
            self.dol = f.read()
        self.units = self._unit_index()
        self._objs: dict[str, elfobj.ElfObject] = {}

    def _unit_index(self) -> list[tuple[int, int, str]]:
        cache = os.path.join(self.root, "build", "match_units.json")
        asm_dir = os.path.join(self.root, "build", "asm")
        if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(asm_dir):
            with open(cache) as f:
                return [tuple(x) for x in json.load(f)]
        head = re.compile(r"^# 0x([0-9A-Fa-f]{8})\.\.0x([0-9A-Fa-f]{8}) \|")
        rows = []
        for name in os.listdir(asm_dir):
            if not name.endswith(".s"):
                continue
            with open(os.path.join(asm_dir, name), encoding="utf-8") as f:
                for line in f:
                    m = head.match(line)
                    if m:
                        rows.append((int(m[1], 16), int(m[2], 16), name[:-2]))
        rows.sort()
        with open(cache, "w") as f:
            json.dump(rows, f)
        return rows

    def lookup(self, key: str) -> TargetSymbol:
        if key in self.symbols:
            return self.symbols[key]
        for old, new in self.renames.items():
            if new == key and old in self.symbols:
                return self.symbols[old]
        try:
            addr = int(key, 16)
        except ValueError:
            raise KeyError(f"no function named {key}") from None
        if addr in self.by_address:
            return self.by_address[addr]
        raise KeyError(f"no symbol at 0x{addr:08X}")

    def display_name(self, s: TargetSymbol) -> str:
        return self.renames.get(s.name, s.name)

    def unit_of(self, address: int) -> str:
        for lo, hi, unit in self.units:
            if lo <= address < hi:
                return unit
        raise KeyError(f"0x{address:08X} is in no split unit")

    def object(self, unit: str) -> elfobj.ElfObject:
        if unit not in self._objs:
            self._objs[unit] = elfobj.read(os.path.join(self.root, "build", "obj", unit + ".o"))
        return self._objs[unit]

    def read(self, address: int, size: int) -> bytes | None:
        for base, length, off, _name in self.dol_sections:
            if base <= address and address + size <= base + length:
                return self.dol[off + address - base:off + address - base + size]
        return None

    def section_kind(self, address: int) -> str | None:
        s = self.by_address.get(address)
        if s is not None:
            return s.section
        best = None
        for t in self.symbols.values():
            if t.address <= address < t.address + max(t.size, 1):
                best = t.section
                break
        return best


# ---------------------------------------------------------------------------------------------------------------
# Compilation
# ---------------------------------------------------------------------------------------------------------------

def compiler(version: str) -> str:
    path = os.path.join(COMPILER_ROOT, version, "mwcceppc.exe")
    if not os.path.exists(path):
        raise FileNotFoundError(path)
    return path


def compile_source(src: str, version: str, flags: str, out_obj: str, includes: list[str]) -> tuple[bool, str]:
    """Compile with short paths: these 2002-era compilers crash (access violation) on long source/output paths
    (found 2026-10-31 with a 104-character source path). The source is copied under a short name into the output
    folder, compiled there with relative names, and its own folder is added as an include path."""
    work = os.path.dirname(os.path.abspath(out_obj))
    ext = os.path.splitext(src)[1]
    key = hashlib.sha1(f"{os.path.abspath(src)}|{version}|{flags}".encode()).hexdigest()[:10]
    short_src, short_obj = f"s{key}{ext}", f"s{key}.o"
    shutil.copyfile(src, os.path.join(work, short_src))
    cmd = [compiler(version), *BASE_FLAGS.split(), *flags.split(), "-i", os.path.dirname(os.path.abspath(src))]
    for inc in includes:
        cmd += ["-i", inc]
    cmd += ["-c", short_src, "-o", short_obj]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", cwd=work)
    ok = r.returncode == 0 and os.path.exists(os.path.join(work, short_obj))
    if ok:
        os.replace(os.path.join(work, short_obj), out_obj)
    os.remove(os.path.join(work, short_src))
    return ok, (r.stdout + r.stderr).strip()


# ---------------------------------------------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------------------------------------------

@dataclass
class Result:
    function: str
    address: int
    target_size: int
    base_size: int | None
    matched: bool
    fuzzy: float
    problems: list[str] = field(default_factory=list)
    diff: list[str] = field(default_factory=list)


def _canon(name: str) -> str:
    return name.replace("@", "__at_")


def _word_mask(relocs: list[elfobj.Reloc], lo: int) -> dict[int, tuple[int, elfobj.Reloc]]:
    """word offset (relative to function start) -> (bits the relocation owns, relocation)."""
    out = {}
    for r in relocs:
        rel = r.offset - lo
        word = rel & ~3
        mask = elfobj.R_MASK.get(r.type)
        if mask is None:
            raise ValueError(f"unsupported relocation type {r.type} at +0x{rel:X}")
        if r.type in (3, 4, 5, 6) and rel & 3 == 0:
            mask <<= 16           # 16-bit field addressed at the high half (data, not an instruction immediate)
        out[word] = (mask, r)
    return out


def read_symbol_map(src: str) -> dict[str, str]:
    """`// @sym <candidate symbol> = <original symbol>` lines: the source's names for original symbols.

    This is the decomp project's symbol rename (what symbols.txt records once a name is known), applied to one
    candidate. It must be one-to-one; a conflict is an error, never a silent overwrite.
    """
    out: dict[str, str] = {}
    seen: dict[str, str] = {}
    pat = re.compile(r"^\s*//\s*@sym\s+(\S+)\s*=\s*(\S+)\s*$")
    with open(src, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            cand, orig = m[1], m[2]
            if out.get(cand, orig) != orig or seen.get(orig, cand) != cand:
                raise ValueError(f"@sym map is not one-to-one at {cand} = {orig}")
            out[cand], seen[orig] = orig, cand
    return out


class Judge:
    """Relocation and data equality between one candidate object and the original image."""

    def __init__(self, project: Project, base: elfobj.ElfObject, symmap: dict[str, str]):
        self.p, self.base, self.symmap = project, base, symmap

    def original_name(self, cand: str) -> str:
        """The original symbol a candidate name stands for; an @sym value may be an address (0x...), which stays valid
        while the project's names improve."""
        orig = self.symmap.get(cand, cand)
        if re.fullmatch(r"0x[0-9A-Fa-f]{8}", orig):
            t = self.p.by_address.get(int(orig, 16))
            orig = t.name if t else orig
        return _canon(orig)

    def names_equal(self, cand: str, orig: str) -> bool:
        orig = _canon(orig)
        cand_orig = self.original_name(cand)
        # a project rename links an older name and the current one; either side may use either
        return cand_orig == orig or self.p.renames.get(orig) == cand_orig or self.p.renames.get(cand_orig) == orig

    def is_local_data(self, sym: elfobj.Symbol) -> bool:
        """Data the candidate defines itself. Its CONTENTS are always compared; when it is also @sym-mapped, the
        original reference must name the mapped symbol too (a mapped vtable is checked slot by slot, not trusted)."""
        return sym.defined and sym.type != elfobj.STT_FUNC and             not self.base.sections[sym.shndx].name.startswith(".text")

    def reloc(self, b: elfobj.Reloc, t_name: str, t_addend: int, t_type: int, depth: int = 0) -> str | None:
        if b.type != t_type:
            return f"relocation type {elfobj.R_NAMES.get(b.type, b.type)} vs {elfobj.R_NAMES.get(t_type, t_type)}"
        bs = b.symbol
        if not self.is_local_data(bs):
            if not self.names_equal(bs.name, t_name):
                return f"targets {self.symmap.get(bs.name, bs.name)}+{b.addend} vs {t_name}+{t_addend}"
            if b.addend != t_addend:
                return f"addend {b.addend} vs {t_addend} on {bs.name}"
            return None
        if bs.name in self.symmap and not self.names_equal(bs.name, t_name):
            return f"targets {self.original_name(bs.name)} vs {t_name}"
        t = self.p.symbols.get(t_name) or self.p.symbols.get(t_name.replace("__at_", "@"))
        if t is None:
            return f"original target {t_name} has no address"
        return self.data(bs, b.addend, t.address + t_addend, t.section, depth)

    def data(self, bs: elfobj.Symbol, addend: int, addr: int, tsec: str, depth: int) -> str | None:
        """Candidate-local data at bs+addend against original bytes at addr, pointers compared by target."""
        sec = self.base.sections[bs.shndx]
        start = bs.value + addend
        if bs.size and bs.type != elfobj.STT_SECTION:
            n = max(bs.size - addend, 1)
        elif sec.type != elfobj.SHT_NOBITS:
            end = sec.data.find(b"\0", start)    # section-relative string literal: through its terminator
            n = (len(sec.data) if end < 0 else end + 1) - start
            text = sec.data[start:start + n - 1]
            # an extent read up to the first zero byte is only known for a string: binary data that happens to begin
            # with zero would otherwise pass after one compared byte (Astra's review, 2026-11-01)
            if end < 0 or not text or any(b < 0x20 and b not in (9, 10, 13) or b > 0x7E for b in text):
                return f"local data {bs.name}+0x{addend:X} has no known extent (no symbol size, not a string)"
        else:
            n = 4
        data = b"\0" * n if sec.type == elfobj.SHT_NOBITS else sec.data[start:start + n]
        if _data_class(sec.name) != _data_class(tsec or ""):
            return f"data section {sec.name} vs original {tsec}"
        want = self.p.read(addr, n)
        if want is None:
            if _data_class(tsec) in ("data", "sdata") and tsec.endswith("bss") and not any(data):
                return None
            return f"original data at 0x{addr:08X} unreadable"
        inner = self.base.relocs_in(bs.shndx, start, start + n)
        masked = bytearray(data)
        want_m = bytearray(want)
        for r in inner:
            k = r.offset - start
            if r.type != 1 or k % 4 or k + 4 > n:
                return f"unsupported data relocation {r.type} at +0x{k:X} in {bs.name}"
            masked[k:k + 4] = want_m[k:k + 4] = b"\0\0\0\0"
        if bytes(masked) != bytes(want_m):
            return f"data bytes {data[:32].hex()} vs original {want[:32].hex()} at 0x{addr:08X}"
        if depth >= 6:
            return None if not inner else f"data pointer nesting too deep at 0x{addr:08X}"
        for r in inner:
            k = r.offset - start
            ptr = struct.unpack_from(">I", want, k)[0]
            tgt = self.p.by_address.get(ptr)
            if tgt is None:
                return f"original pointer 0x{ptr:08X} at 0x{addr + k:08X} names no symbol"
            if self.is_local_data(r.symbol):
                why = self.data(r.symbol, r.addend, ptr, tgt.section, depth + 1)
            else:
                why = self.reloc(r, tgt.name, 0, 1, depth + 1)
            if why:
                return f"in {bs.name}+0x{k:X}: {why}"
        return None


def compare(project: Project, base: elfobj.ElfObject, base_sym: elfobj.Symbol, target: TargetSymbol,
            symmap: dict[str, str]) -> Result:
    judge = Judge(project, base, symmap)
    unit = project.unit_of(target.address)
    tobj = project.object(unit)
    tsym = tobj.find_symbol(target.name)
    if tsym is None:
        raise KeyError(f"{target.name} not defined in {unit}.o")
    tsec = tobj.sections[tsym.shndx]
    tbytes = tsec.data[tsym.value:tsym.value + tsym.size]
    trel = _word_mask(tobj.relocs_in(tsym.shndx, tsym.value, tsym.value + tsym.size), tsym.value)
    bsec = base.sections[base_sym.shndx]
    bbytes = bsec.data[base_sym.value:base_sym.value + base_sym.size]
    brel = _word_mask(base.relocs_in(base_sym.shndx, base_sym.value, base_sym.value + base_sym.size), base_sym.value)

    res = Result(project.display_name(target), target.address, len(tbytes), len(bbytes), False, 0.0)
    if len(bbytes) != len(tbytes):
        res.problems.append(f"M1 size 0x{len(bbytes):X} vs original 0x{len(tbytes):X}")
    # M3 in full: the same relocations (offset, type), as many on each side, at most one per word (the word mask
    # keeps one; a second would go unchecked - Astra's review, 2026-11-01)
    t_all = sorted((r.offset - tsym.value, r.type) for r in tobj.relocs_in(tsym.shndx, tsym.value,
                                                                            tsym.value + tsym.size))
    b_all = sorted((r.offset - base_sym.value, r.type) for r in base.relocs_in(base_sym.shndx, base_sym.value,
                                                                               base_sym.value + base_sym.size))
    if t_all != b_all:
        res.problems.append(f"M3 relocations differ: {len(b_all)} vs original {len(t_all)} "
                            f"(first difference {next((x for x in b_all if x not in t_all), None)} / "
                            f"{next((x for x in t_all if x not in b_all), None)})")
    for rels in (t_all, b_all):
        words_with = [o & ~3 for o, _ in rels]
        if len(words_with) != len(set(words_with)):
            res.problems.append("M3 two relocations in one word: not supported, not matched")
            break

    def words(data: bytes, rel) -> list[tuple]:
        out = []
        for i in range(0, len(data) - 3, 4):
            w = struct.unpack_from(">I", data, i)[0]
            if i in rel:
                mask, r = rel[i]
                out.append((w & ~mask & 0xFFFFFFFF, r.type))
            else:
                out.append((w, None))
        return out

    bw, tw = words(bbytes, brel), words(tbytes, trel)
    sm = difflib.SequenceMatcher(a=tw, b=bw, autojunk=False)
    res.fuzzy = 100.0 * 2 * sum(b.size for b in sm.get_matching_blocks()) / max(len(tw) + len(bw), 1)

    for i in range(min(len(bw), len(tw))):
        off = i * 4
        if bw[i] != tw[i]:
            res.problems.append(f"M2/M3 word +0x{off:X}: {bbytes[off:off + 4].hex()} vs original {tbytes[off:off + 4].hex()}")
        elif off in brel:
            tr = trel[off][1]
            why = judge.reloc(brel[off][1], tr.symbol.name, tr.addend, tr.type)
            if why:
                res.problems.append(f"M4 +0x{off:X}: {why}")
    for i, op in enumerate(sm.get_opcodes()):
        tag, a0, a1, b0, b1 = op
        if tag != "equal":
            res.diff.append(f"{tag:7s} original[{a0}:{a1}] {' '.join(tbytes[k*4:k*4+4].hex() for k in range(a0, min(a1, a0 + 4)))}"
                            f"  | candidate[{b0}:{b1}] {' '.join(bbytes[k*4:k*4+4].hex() for k in range(b0, min(b1, b0 + 4)))}")
    res.matched = not res.problems
    if res.matched:
        res.fuzzy = 100.0
    return res


def objdiff_check(base_obj: str, base: elfobj.ElfObject, base_sym: elfobj.Symbol, project: Project,
                  target: TargetSymbol, symmap: dict[str, str]) -> tuple[float | None, list[int]]:
    """Independent cross-check with objdiff-cli on the same pair. The candidate copy gets the @sym renames (the same
    renames the matcher applies); returns objdiff's match percent and the instruction indices it marks different."""
    unit = project.unit_of(target.address)
    judge = Judge(project, base, symmap)
    renames = {k: judge.original_name(k).replace("__at_", "@") if k in symmap else k for k in symmap}
    renames[base_sym.name] = target.name
    present = {s.name for s in base.symbols}
    with tempfile.TemporaryDirectory(dir=os.path.dirname(base_obj)) as tmp:
        renamed = os.path.join(tmp, "base.o")
        args = [f"--redefine-sym={k}={v}" for k, v in renames.items() if k in present and k != v]
        subprocess.run([OBJCOPY, *args, base_obj, renamed], check=True)
        out = os.path.join(tmp, "diff.json")
        r = subprocess.run([OBJDIFF, "diff", "-1", os.path.join(project.root, "build", "obj", unit + ".o"),
                            "-2", renamed, target.name, "-o", out, "--format", "json"], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            return None, []
        with open(out, encoding="utf-8") as f:
            doc = json.load(f)
    for side in doc.get("left", {}).get("symbols", []):
        if side.get("name") == target.name:
            bad = [i for i, ins in enumerate(side.get("instructions", []))
                   if ins.get("diff_kind", "DIFF_NONE") != "DIFF_NONE"]
            return side.get("match_percent"), bad
    return None, []


# ---------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------

def pair_functions(project: Project, base: elfobj.ElfObject, wanted: list[str],
                   symmap: dict[str, str]) -> list[tuple[elfobj.Symbol, TargetSymbol]]:
    """Candidate functions paired with original functions. Without --fn: every defined function whose (mapped) name
    is an original symbol. A weak function (an inline the compiler emitted out of line) pairs only when mapped."""
    pairs = []
    for bs in base.functions():
        orig = symmap.get(bs.name, bs.name)
        if wanted and bs.name not in wanted and orig not in wanted:
            continue
        if not wanted and bs.bind == elfobj.STB_WEAK and bs.name not in symmap:
            continue
        try:
            pairs.append((bs, project.lookup(orig)))
        except KeyError:
            if wanted:
                raise
    missing = [w for w in wanted if not any(w in (bs.name, ts.name) or project.display_name(ts) == w
                                            for bs, ts in pairs)]
    if missing:
        raise KeyError(f"candidate object defines no function {missing[0]}")
    return pairs


def run_one(project: Project, src: str, version: str, flags: str, wanted: list[str], workdir: str,
            includes: list[str], symmap: dict[str, str]) -> tuple[list[Result] | None, str, list]:
    tag = re.sub(r"[^A-Za-z0-9]+", "_", f"{version}_{flags}")
    obj = os.path.join(workdir, f"{os.path.splitext(os.path.basename(src))[0]}__{tag}.o")
    ok, log = compile_source(src, version, flags, obj, includes)
    if not ok:
        return None, log, []
    base = elfobj.read(obj)
    present = {s.name for s in base.symbols}
    unused = sorted(k for k in symmap if k not in present)
    if unused:
        print(f"warning: @sym names the object does not contain (misspelled mangling?): {', '.join(unused[:5])}",
              file=sys.stderr)
    pairs = pair_functions(project, base, wanted, symmap)
    return [compare(project, base, bs, ts, symmap) for bs, ts in pairs], obj, pairs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("source")
    ap.add_argument("--fn", action="append", default=[], help="function to judge (default: every function defined)")
    ap.add_argument("--cc", default="2.6")
    ap.add_argument("--flags", default=FLAG_SETS["O4p"])
    ap.add_argument("--sweep", action="store_true", help="every compiler version x every flag set")
    ap.add_argument("--extra", default="", help="flags appended to every configuration (e.g. -str readonly)")
    ap.add_argument("--include", action="append", default=[])
    ap.add_argument("--json")
    ap.add_argument("--objdiff", action="store_true", help="cross-check matched functions with objdiff-cli")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args(argv)

    project = Project(args.project)
    symmap = read_symbol_map(args.source)
    workdir = os.path.join(args.project, "build", "match")
    os.makedirs(workdir, exist_ok=True)
    configs = list(itertools.product(VERSIONS, FLAG_SETS.items())) if args.sweep else [(args.cc, ("custom", args.flags))]
    if args.extra:
        configs = [(v, (label, f"{flags} {args.extra}")) for v, (label, flags) in configs]
    table = []
    for version, (label, flags) in configs:
        results, info, pairs = run_one(project, args.source, version, flags, args.fn, workdir, args.include, symmap)
        if results is None:
            table.append({"cc": version, "flags": label, "error": info.splitlines()[-1] if info else "compile failed"})
            if not args.sweep:
                print(info)
            continue
        row = {"cc": version, "flags": label, "matched": sum(r.matched for r in results), "functions": len(results),
               "fuzzy": round(sum(r.fuzzy for r in results) / max(len(results), 1), 2),
               "results": [r.__dict__ for r in results]}
        if args.objdiff:
            base = elfobj.read(info)
            for k, (bs, ts) in enumerate(pairs):
                pct, bad = objdiff_check(info, base, bs, project, ts, symmap)
                judge = Judge(project, base, symmap)
                lits = {(r.offset - bs.value) // 4 for r in base.relocs_in(bs.shndx, bs.value, bs.value + bs.size)
                        if judge.is_local_data(r.symbol)}
                unexplained = [i for i in bad if i not in lits]
                row["results"][k]["objdiff"] = pct
                row["results"][k]["objdiff_literal_only"] = len(bad) - len(unexplained)
                if unexplained and results[k].matched:
                    results[k].matched = False
                    results[k].problems.append(f"X1 objdiff marks instructions {unexplained} different")
                    row["results"][k].update(matched=False, problems=results[k].problems)
            row["matched"] = sum(r.matched for r in results)
        table.append(row)
        if not args.sweep or args.verbose:
            for r in results:
                rr = row["results"][results.index(r)]
                od = rr.get("objdiff")
                note = ""
                if od is not None:
                    note = f"  objdiff={od:.2f}%"
                    if rr.get("objdiff_literal_only"):
                        note += f" ({rr['objdiff_literal_only']} literal operands it cannot read; bytes checked in main.dol)"
                print(f"{'MATCH' if r.matched else 'diff '} {r.fuzzy:6.2f}%  0x{r.address:08X} {r.function}{note}")
                for p in r.problems[:6]:
                    print("        ", p)
                if not r.matched:
                    for d in r.diff[:6]:
                        print("        ", d)
    if args.sweep:
        good = [t for t in table if "error" not in t]
        good.sort(key=lambda t: (-t["matched"], -t["fuzzy"]))
        for t in good[:15]:
            print(f"{t['cc']:9s} {t['flags']:14s} matched {t['matched']}/{t['functions']}  mean fuzzy {t['fuzzy']:.2f}%")
        bad = [t for t in table if "error" in t]
        if bad:
            print(f"{len(bad)} configurations failed to compile, e.g. {bad[0]['cc']} {bad[0]['flags']}: {bad[0]['error']}")
    if args.sweep:
        per: dict[str, list[str]] = {}
        for t in table:
            if "error" not in t and t["matched"] == t["functions"] and t["functions"]:
                per.setdefault(t["cc"], []).append(t["flags"])
        print("all functions matched under:", "; ".join(f"{k} [{', '.join(v)}]" for k, v in per.items()) or "none")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(table, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
