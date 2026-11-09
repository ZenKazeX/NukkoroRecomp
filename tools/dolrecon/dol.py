"""DOL import, validation and layout manifest (docs/DOLRECON.md section 5).

A DOL file is a 0x100-byte header followed by up to 7 text and 11 data sections at file offsets the header declares.
This module never confuses the two address spaces: every region carries a file offset; only sections also carry a
memory address. BSS is declared in the header and has no file bytes.

Ownership: `layout()` partitions [0, file size) into regions - header, sections, and every gap / padding / trailing
range between them - so that each original file byte has exactly one owner. Overlapping or out-of-bounds sections are
reported as diagnostics and make the layout invalid (no silent best effort).
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field, asdict

HEADER_SIZE = 0x100
TEXT_COUNT, DATA_COUNT = 7, 11
SECTION_COUNT = TEXT_COUNT + DATA_COUNT
# Header field offsets (big-endian u32 arrays).
OFF_OFFSETS, OFF_ADDRS, OFF_SIZES = 0x00, 0x48, 0x90
OFF_BSS_ADDR, OFF_BSS_SIZE, OFF_ENTRY = 0xD8, 0xDC, 0xE0
HEADER_USED_END = 0xE4  # 0xE4..0xFF: padding, preserved as header bytes


@dataclass(frozen=True)
class Section:
    index: int            # 0-6 text, 7-17 data (header order)
    kind: str             # "text" | "data"
    name: str             # ".text0" ... ".data10"
    file_offset: int
    address: int
    size: int

    @property
    def file_end(self) -> int:
        return self.file_offset + self.size

    @property
    def address_end(self) -> int:
        return self.address + self.size


@dataclass(frozen=True)
class Region:
    """A physical byte range of the file with exactly one owner."""
    file_offset: int
    size: int
    kind: str             # "header" | "section" | "gap" | "trailing"
    name: str
    address: int | None = None  # memory address when the region is (part of) a loaded section


@dataclass
class DolImage:
    raw: bytes
    sections: list[Section]
    bss_address: int
    bss_size: int
    entry: int
    diagnostics: list[str] = field(default_factory=list)

    # ---- identity ----------------------------------------------------------------------------------------------
    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.raw).hexdigest()

    # ---- address <-> file offset (never implicit) -------------------------------------------------------------
    def section_at_address(self, address: int) -> Section | None:
        for s in self.sections:
            if s.address <= address < s.address_end:
                return s
        return None

    def file_offset_of(self, address: int) -> int | None:
        s = self.section_at_address(address)
        return None if s is None else s.file_offset + (address - s.address)

    def read(self, address: int, size: int) -> bytes:
        s = self.section_at_address(address)
        if s is None or address + size > s.address_end:
            raise ValueError(f"{address:#010x}+{size:#x} is not inside one loaded section")
        off = s.file_offset + (address - s.address)
        return self.raw[off:off + size]

    def word(self, address: int) -> int:
        return struct.unpack(">I", self.read(address, 4))[0]

    def in_bss(self, address: int) -> bool:
        return self.bss_size != 0 and self.bss_address <= address < self.bss_address + self.bss_size

    # ---- byte ownership -------------------------------------------------------------------------------------
    def layout(self) -> list[Region]:
        """Every file byte exactly once, in file order. Raises if the sections overlap in the file."""
        size = len(self.raw)
        claims = [(0, HEADER_SIZE, "header", "header", None)]
        for s in sorted(self.sections, key=lambda s: s.file_offset):
            claims.append((s.file_offset, s.size, "section", s.name, s.address))
        claims.sort(key=lambda c: c[0])
        regions: list[Region] = []
        pos = 0
        for off, length, kind, name, addr in claims:
            if off < pos:
                raise ValueError(f"{name} at file offset {off:#x} overlaps the previous owner (ends {pos:#x})")
            if off > pos:
                regions.append(Region(pos, off - pos, "gap", f"gap_{pos:06x}"))
            regions.append(Region(off, length, kind, name, addr))
            pos = off + length
        if pos < size:
            regions.append(Region(pos, size - pos, "trailing", f"trailing_{pos:06x}"))
        assert sum(r.size for r in regions) == size
        return regions

    def manifest(self, config: dict | None = None) -> dict:
        regions = self.layout()
        return {
            "schema": "dolrecon.layout/1",
            "input": {"sha256": self.sha256, "file_size": len(self.raw)},
            "entry": f"{self.entry:#010x}",
            "bss": {"address": f"{self.bss_address:#010x}", "size": self.bss_size},
            "sections": [{**asdict(s), "file_offset": f"{s.file_offset:#x}", "address": f"{s.address:#010x}",
                          "sha256": hashlib.sha256(self.raw[s.file_offset:s.file_end]).hexdigest()}
                         for s in self.sections],
            "regions": [{"file_offset": f"{r.file_offset:#x}", "size": r.size, "kind": r.kind, "name": r.name,
                         "address": None if r.address is None else f"{r.address:#010x}",
                         "sha256": hashlib.sha256(self.raw[r.file_offset:r.file_offset + r.size]).hexdigest()}
                        for r in regions],
            "diagnostics": list(self.diagnostics),
            "config": config or {},
        }


def load(raw: bytes) -> DolImage:
    """Parses and validates. Structural errors raise; suspicious but loadable things go to diagnostics."""
    if len(raw) < HEADER_SIZE:
        raise ValueError(f"file is {len(raw)} bytes, smaller than the 0x100-byte DOL header")

    def u32s(at: int) -> tuple[int, ...]:
        return struct.unpack_from(f">{SECTION_COUNT}I", raw, at)

    offsets, addrs, sizes = u32s(OFF_OFFSETS), u32s(OFF_ADDRS), u32s(OFF_SIZES)
    bss_address, bss_size, entry = struct.unpack_from(">III", raw, OFF_BSS_ADDR)
    diagnostics: list[str] = []
    sections: list[Section] = []
    for i in range(SECTION_COUNT):
        kind = "text" if i < TEXT_COUNT else "data"
        name = f".{kind}{i if i < TEXT_COUNT else i - TEXT_COUNT}"
        off, addr, size = offsets[i], addrs[i], sizes[i]
        if size == 0:
            if off or addr:
                diagnostics.append(f"{name}: size 0 but offset {off:#x} / address {addr:#x} set")
            continue
        if off < HEADER_SIZE:
            raise ValueError(f"{name}: file offset {off:#x} inside the header")
        if off + size > len(raw):
            raise ValueError(f"{name}: file range {off:#x}+{size:#x} past the end of the file ({len(raw):#x})")
        if addr + size > 0x1_0000_0000:
            raise ValueError(f"{name}: address range {addr:#x}+{size:#x} wraps 32 bits")
        if kind == "text" and (addr % 4 or size % 4 or off % 4):
            diagnostics.append(f"{name}: text not word aligned (offset {off:#x}, address {addr:#x}, size {size:#x})")
        sections.append(Section(i, kind, name, off, addr, size))
    # Memory overlaps between sections are a conflicting mapping.
    by_addr = sorted(sections, key=lambda s: s.address)
    for a, b in zip(by_addr, by_addr[1:]):
        if b.address < a.address_end:
            raise ValueError(f"{a.name} and {b.name} overlap in memory ({a.address:#x}+{a.size:#x}, {b.address:#x})")
    if bss_size:
        for s in sections:
            if s.address < bss_address + bss_size and bss_address < s.address_end:
                # Common in real DOLs: small-data sections (.sdata) sit inside the declared BSS span.
                diagnostics.append(f"{s.name} lies inside the declared BSS span ({bss_address:#x}+{bss_size:#x})")
    image = DolImage(raw, sections, bss_address, bss_size, entry, diagnostics)
    if not any(s.kind == "text" and s.address <= entry < s.address_end for s in sections):
        raise ValueError(f"entry point {entry:#x} is not inside a text section")
    image.layout()  # raises on file-offset overlaps
    if any(raw[HEADER_USED_END:HEADER_SIZE]):
        diagnostics.append("header padding 0xE4-0xFF is not all zero (preserved as is)")
    return image


def load_file(path: str) -> DolImage:
    with open(path, "rb") as f:
        return load(f.read())
