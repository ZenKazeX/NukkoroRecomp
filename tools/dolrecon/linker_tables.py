"""The CodeWarrior linker's generated tables, rebuilt from the reconstructed layout (docs/DOLRECON.md "Accuracy").

dtk leaves three linker-generated objects out of its assembly (they reference the final layout). Kept as raw bytes
they are opaque; generated here from the layout they become meaning, and the rebuild still checks every byte:

  _rom_copy_info   {rom address, ram address, size} for every loaded output section in address order, then a zero
                   triple. size = the section's content end (its last assembly block, or the tables themselves for
                   the section that holds them) minus its start - not the DOL header's padded size.
  _bss_init_info   {address, size} for every uninitialised section in address order, then a zero pair.
  _eti_init_info   {extabindex start, extabindex entries end, first function covered, covered code size}, then a zero
                   quad; the covered range is min(start) .. max(start + size) over the extabindex entries.

`generate` returns the tables only when the model reproduces the original bytes exactly; otherwise it reports the
first differing word and the caller keeps the bytes as a preserved fragment (never silently "fixed").
"""

from __future__ import annotations

import struct

BSS = {".bss", ".sbss", ".sbss2"}
TABLE_SIZES = {"_rom_copy_info": None, "_bss_init_info": None, "_eti_init_info": None}


# The linker ends the constructor and destructor lists with a null word (the runtime walks them to it); dtk does not
# emit it. It is part of the section's content size in _rom_copy_info.
TERMINATED = (".ctors", ".dtors")


def _section_extents(blocks) -> dict[str, tuple[int, int]]:
    ext: dict[str, list[int]] = {}
    for b in blocks:
        lo, hi = ext.get(b.section, [b.start, b.end])
        ext[b.section] = [min(lo, b.start), max(hi, b.end)]
    return {k: (v[0], v[1]) for k, v in ext.items()}


def terminators(image, blocks) -> list[tuple[int, bytes, str, str]]:
    """The null word after .ctors / .dtors, where the original has one there."""
    out = []
    for sec, (lo, hi) in _section_extents(blocks).items():
        if sec in TERMINATED and image.read(hi, 4) == bytes(4):
            out.append((hi, bytes(4), f"{sec} null terminator", "linker appends a null word to the list"))
    return out


def generate(image, blocks, symbols: dict[str, tuple[int, str]]) -> dict:
    """symbols: name -> (address, section) from symbols.txt. Returns {"tables": [(address, bytes, name, derivation)],
    "rejected": [{name, reason}]}."""
    out = {"tables": [], "rejected": []}
    ext = _section_extents(blocks)
    where = {n: symbols[n] for n in TABLE_SIZES if n in symbols}

    terms = {addr for addr, _, _, _ in terminators(image, blocks)}

    def section_end(sec: str) -> int:
        end = ext[sec][1] + (4 if ext[sec][1] in terms else 0)
        # The section holding generated tables ends after them.
        for n, (addr, s) in where.items():
            if s == sec:
                end = max(end, addr + table_len(n))
        return end

    loaded = sorted((lo, s) for s, (lo, _) in ext.items() if s not in BSS)
    bss = sorted((lo, s) for s, (lo, _) in ext.items() if s in BSS)

    def table_len(n: str) -> int:
        if n == "_rom_copy_info":
            return 12 * (len(loaded) + 1)
        if n == "_bss_init_info":
            return 8 * (len(bss) + 1)
        return 16 + 16  # _eti_init_info: one quad + the zero quad

    candidates = []
    if "_rom_copy_info" in where:
        words = []
        for lo, s in loaded:
            words += [lo, lo, section_end(s) - lo]
        words += [0, 0, 0]
        candidates.append(("_rom_copy_info", words, "loaded sections in address order: (rom, ram, content size)"))
    if "_bss_init_info" in where:
        words = []
        for lo, s in bss:
            words += [lo, ext[s][1] - lo]
        words += [0, 0]
        candidates.append(("_bss_init_info", words, "uninitialised sections in address order: (address, size)"))
    if "_eti_init_info" in where:
        eti_sec = "extabindex"
        if eti_sec in ext:
            lo, hi = ext[eti_sec]
            starts, ends = [], []
            for at in range(lo, hi - 11, 12):
                f, size, _ = struct.unpack(">III", image.read(at, 12))
                starts.append(f)
                ends.append(f + size)
            if starts:
                words = [lo, hi, min(starts), max(ends) - min(starts), 0, 0, 0, 0]
                candidates.append(("_eti_init_info", words,
                                   "(extabindex start, entries end, first covered function, covered code size)"))
    out["tables"] += terminators(image, blocks)
    for name, words, derivation in candidates:
        addr = where[name][0]
        blob = struct.pack(f">{len(words)}I", *words)
        orig = image.read(addr, len(blob))
        if blob == orig:
            out["tables"].append((addr, blob, name, derivation))
        else:
            i = next(i for i in range(0, len(blob), 4) if blob[i:i + 4] != orig[i:i + 4])
            out["rejected"].append({"name": name, "address": f"{addr:#010x}",
                                    "first_difference": f"{addr + i:#010x}", "model": blob[i:i + 4].hex(),
                                    "original": orig[i:i + 4].hex()})
    return out
