"""Minimal ELF32 big-endian relocatable-object reader (PowerPC).

Reads what matching needs: sections with their bytes, the symbol table and RELA relocations. Both object kinds the
matcher compares are read with this one reader: GNU as objects assembled from the dtk split (target) and mwcceppc
objects compiled from candidate source (base).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

SHT_SYMTAB, SHT_RELA, SHT_NOBITS, SHT_REL = 2, 4, 8, 9
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE = 0, 1, 2, 3, 4

# PowerPC relocation types used by GameCube code.
R_NAMES = {0: "NONE", 1: "ADDR32", 2: "ADDR24", 3: "ADDR16", 4: "ADDR16_LO", 5: "ADDR16_HI", 6: "ADDR16_HA",
           7: "ADDR14", 10: "REL24", 11: "REL14", 109: "EMB_SDA21", 26: "REL32"}
# Bits of the 32-bit word at r_offset that each relocation type rewrites.
R_MASK = {1: 0xFFFFFFFF, 2: 0x03FFFFFC, 3: 0x0000FFFF, 4: 0x0000FFFF, 5: 0x0000FFFF, 6: 0x0000FFFF,
          7: 0x0000FFFC, 10: 0x03FFFFFC, 11: 0x0000FFFC, 26: 0xFFFFFFFF,
          109: 0x001FFFFF}   # SDA21 rewrites the base register field (r0/r2/r13) and the 16-bit offset


@dataclass
class Section:
    index: int
    name: str
    type: int
    flags: int
    addralign: int
    data: bytes
    size: int
    link: int
    info: int


@dataclass
class Symbol:
    index: int
    name: str
    value: int
    size: int
    bind: int
    type: int
    shndx: int

    @property
    def defined(self) -> bool:
        return self.shndx not in (SHN_UNDEF, SHN_COMMON)


@dataclass
class Reloc:
    offset: int
    type: int
    symbol: Symbol
    addend: int


@dataclass
class ElfObject:
    path: str
    sections: list[Section] = field(default_factory=list)
    symbols: list[Symbol] = field(default_factory=list)
    relocs: dict[int, list[Reloc]] = field(default_factory=dict)   # target section index -> sorted relocations

    def section_by_name(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def find_symbol(self, name: str) -> Symbol | None:
        hits = [s for s in self.symbols if s.name == name and s.defined]
        return hits[0] if hits else None

    def functions(self) -> list[Symbol]:
        return [s for s in self.symbols if s.type == STT_FUNC and s.defined]

    def relocs_in(self, shndx: int, lo: int, hi: int) -> list[Reloc]:
        return [r for r in self.relocs.get(shndx, []) if lo <= r.offset < hi]


def read(path: str) -> ElfObject:
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 2:
        raise ValueError(f"{path}: not an ELF32 big-endian object")
    (e_shoff,) = struct.unpack_from(">I", raw, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", raw, 0x2E)
    heads = [struct.unpack_from(">IIIIIIIIII", raw, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    strtab_off = heads[e_shstrndx][4]

    def cstr(off: int) -> str:
        end = raw.index(b"\0", off)
        return raw[off:end].decode("latin-1")

    obj = ElfObject(path)
    for i, (name, typ, flags, _addr, off, size, link, info, align, _ent) in enumerate(heads):
        data = b"" if typ == SHT_NOBITS else raw[off:off + size]
        obj.sections.append(Section(i, cstr(strtab_off + name), typ, flags, align, data, size, link, info))

    symtab = next((s for s in obj.sections if s.type == SHT_SYMTAB), None)
    if symtab is not None:
        str_off = heads[symtab.link][4]
        for i in range(len(symtab.data) // 16):
            name, value, size, info, _other, shndx = struct.unpack_from(">IIIBBH", symtab.data, i * 16)
            obj.symbols.append(Symbol(i, cstr(str_off + name), value, size, info >> 4, info & 0xF, shndx))

    for s in obj.sections:
        if s.type == SHT_REL:
            raise ValueError(f"{path}: REL (no addend) relocations are not supported")
        if s.type != SHT_RELA:
            continue
        rows = []
        for i in range(len(s.data) // 12):
            off, info, addend = struct.unpack_from(">IIi", s.data, i * 12)
            rows.append(Reloc(off, info & 0xFF, obj.symbols[info >> 8], addend))
        obj.relocs.setdefault(s.info, []).extend(rows)
    for rows in obj.relocs.values():
        rows.sort(key=lambda r: r.offset)
    return obj
