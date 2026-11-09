#!/usr/bin/env python3
"""Pins the RwDevice-surface native owners (rwR1, docs/agents/vi.md "Step 4").

Each owner (runtime/scaffold/rw_driver/rw_driver.cpp install_device_surface_owners) must call the SAME scaffold function
the lowered sites of that entry call (extract/lift-sites-r2.txt), with the sites' argument count taken from r3 upward in
order, and must be declared in extract/native-owners-r2.txt. The accessor must return what the original lis/addi/blr
words compute (read from main.dol). A self-test mutates the source text and requires each defect to be detected.

usage: test_device_surface_owners.py <main.dol> [--selftest-only]
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "runtime/scaffold/rw_driver/rw_driver.cpp"
SITES = ROOT / "extract/lift-sites-r2.txt"
OWNERS = ROOT / "extract/native-owners-r2.txt"

# address -> the scaffold function the lowered sites of that entry call (the table word / call target in the DOL)
EXPECTED = {
    0x80498954: "rw_render_state_set",
    0x804984C4: "rw_render_state_get",
    0x804956F4: "rw_camera_begin_update",
    0x80495CAC: "rw_camera_end_update",
    0x80495638: "rw_camera_clear",
    0x80491E80: "rw_im2d_render_primitive",
    0x8049245C: "rw_im2d_render_indexed_primitive",
    0x80496208: "rw_camera_texture_copy",
}
ACCESSOR = 0x804960F0


def site_argc(text):
    counts = {}
    for line in text.splitlines():
        m = re.match(r"site\s+0x[0-9A-Fa-f]+\s+0x[0-9A-Fa-f]+\s+call\s+(\w+)\s+(\d+)", line)
        if m:
            counts.setdefault(m.group(1), set()).add(int(m.group(2)))
    return counts


def parse_owners(source):
    out = {}
    for m in re.finditer(r'cpu\.install_hle\((0x[0-9A-Fa-f]+)u, \[\]\(Cpu& c\) \{(.*?)\n    \}, "([^"]*)"\);', source, re.S):
        out[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return out


def check(source, sites, owners_list, dol):
    errors = []
    argc = site_argc(sites)
    owners = parse_owners(source)
    if set(owners) != set(EXPECTED) | {ACCESSOR}:
        errors.append(f"owner set differs: {sorted(hex(a) for a in set(owners) ^ (set(EXPECTED) | {ACCESSOR}))}")
    listed = {int(m.group(1), 16) for m in re.finditer(r"^(0x[0-9A-Fa-f]+)\s", owners_list, re.M)}
    for address in set(EXPECTED) | {ACCESSOR}:
        if address not in listed:
            errors.append(f"{address:#x} missing from native-owners-r2.txt")
    for address, name in EXPECTED.items():
        if address not in owners:
            continue
        body = owners[address][0]
        m = re.search(r"\b(rw_\w+)\(c((?:, c\.gpr\[\d+\])*)\)", body)
        if not m or m.group(1) != name:
            errors.append(f"{address:#x}: calls {m.group(1) if m else None}, expected {name}")
            continue
        regs = [int(r) for r in re.findall(r"c\.gpr\[(\d+)\]", m.group(2))]
        if regs != list(range(3, 3 + len(regs))):
            errors.append(f"{address:#x}: arguments are not r3.. in order: {regs}")
        want = argc.get(name)
        if want is None or len(want) != 1 or len(regs) != next(iter(want)):
            errors.append(f"{address:#x}: {len(regs)} argument(s), the lowered sites of {name} pass {want}")
        assigns = re.search(r"c\.gpr\[3\] = rw_", body) is not None
        void_sites = any(re.search(rf"call\s+{name}\s+\d+\s+void", l) for l in sites.splitlines())
        if void_sites == assigns:
            errors.append(f"{address:#x}: result handling differs from the sites (void={void_sites}, assigns r3={assigns})")
    # the accessor: lis r3, hi; addi r3, r3, lo; blr
    if ACCESSOR in owners:
        m = re.search(r"c\.gpr\[3\] = (0x[0-9A-Fa-f]+)u;", owners[ACCESSOR][0])
        value = int(m.group(1), 16) if m else None
        if dol is not None:
            words = [dol(ACCESSOR + 4 * i) for i in range(3)]
            lis, addi, blr = words
            if not (lis >> 16 == 0x3C60 and addi >> 16 == 0x3863 and blr == 0x4E800020):
                errors.append(f"accessor words are {[hex(w) for w in words]}")
            else:
                signed_lo = (addi & 0xFFFF) - (0x10000 if addi & 0x8000 else 0)
                expect = (((lis & 0xFFFF) << 16) + signed_lo) & 0xFFFFFFFF
                if value != expect:
                    errors.append(f"accessor returns {value and hex(value)}, the original computes {expect:#x}")
    return errors


def load_dol(path):
    data = Path(path).read_bytes()
    sections = []
    for i in range(18):
        off, addr, size = (struct.unpack(">I", data[o + 4 * i:o + 4 * i + 4])[0] for o in (0, 0x48, 0x90))
        if size:
            sections.append((off, addr, size))

    def word(address):
        for off, addr, size in sections:
            if addr <= address < addr + size:
                return struct.unpack(">I", data[off + address - addr:off + address - addr + 4])[0]
        raise KeyError(hex(address))

    return word


def main(argv):
    dol_path = argv[1] if len(argv) > 1 and not argv[1].startswith("--") else None
    source = SOURCE.read_text(encoding="utf-8")
    sites = SITES.read_text(encoding="utf-8")
    owners_list = OWNERS.read_text(encoding="utf-8")
    dol = load_dol(dol_path) if dol_path else None
    failures = 0
    if "--selftest-only" not in argv:
        for e in check(source, sites, owners_list, dol):
            print("FAIL", e)
            failures += 1
    # negative controls: every mutation must be caught
    mutations = {
        "wrong scaffold function": (source.replace("rw_render_state_set(c, c.gpr[3], c.gpr[4])", "rw_render_state_get(c, c.gpr[3], c.gpr[4])", 1), owners_list),
        "dropped argument": (source.replace("rw_camera_begin_update(c, c.gpr[3], c.gpr[4], c.gpr[5])", "rw_camera_begin_update(c, c.gpr[3], c.gpr[4])", 1), owners_list),
        "wrong argument register": (source.replace("rw_im2d_render_primitive(c, c.gpr[3], c.gpr[4], c.gpr[5])", "rw_im2d_render_primitive(c, c.gpr[4], c.gpr[3], c.gpr[5])", 1), owners_list),
        "wrong accessor value": (source.replace("c.gpr[3] = 0x8056F50Cu;", "c.gpr[3] = 0x8056F510u;", 1), owners_list),
        "owner not declared": (source, owners_list.replace("0x80498954 GcnSetRenderState", "", 1)),
    }
    for name, (src, lst) in mutations.items():
        if src == source and lst == owners_list:
            print("FAIL selftest mutation did not apply:", name)
            failures += 1
        elif not check(src, sites, lst, dol):
            print("FAIL selftest: not detected:", name)
            failures += 1
    print(f"device surface owners: {'OK' if failures == 0 else str(failures) + ' failure(s)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
