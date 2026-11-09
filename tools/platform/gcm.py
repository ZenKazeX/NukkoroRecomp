"""The GameCube disc image (GCM) of Shadow the Hedgehog, rebuilt reproducibly from the extracted disc tree.

    python -B gcm.py check <disc tree>               the FST against the extracted files (no output written)
    python -B gcm.py build <disc tree> <image> [--dry-run]

<disc tree> holds sys/ (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin) and files/, as extracted from the disc.
The image is the console's medium: boot.bin at 0, bi2.bin at 0x440, the apploader at 0x2440, main.dol at the offset
boot.bin 0x420 gives, fst.bin at boot.bin 0x424 (size 0x428), and every file at the disc offset its FST entry gives;
gaps are zero. Nothing is invented: every offset comes from the original boot.bin / fst.bin, and a file whose size
disagrees with its FST entry, an entry without a file, a file without an entry, or two extents that overlap stop
the build. The image's sha256 is printed and written beside it (<image>.sha256) so a rebuild can be compared.
"""
from __future__ import annotations

import hashlib
import os
import struct
import sys

SECTOR = 0x8000


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def read_fst(fst: bytes):
    """[(path, is_dir, offset, length)] in FST order (entry 0 = root)."""
    count = be32(fst, 8)
    names = count * 12
    out, stack = [], [("", count)]       # (directory path, end entry)
    i = 1
    while i < count:
        while stack and i >= stack[-1][1]:
            stack.pop()
        w0, a, b = be32(fst, i * 12), be32(fst, i * 12 + 4), be32(fst, i * 12 + 8)
        is_dir = w0 >> 24 != 0
        off = names + (w0 & 0xFFFFFF)
        end = fst.index(b"\0", off)
        name = fst[off:end].decode("shift_jis")
        path = (stack[-1][0] + "/" + name) if stack and stack[-1][0] else name
        if is_dir:
            out.append((path, True, a, b))           # a = parent entry, b = end entry
            stack.append((path, b))
        else:
            out.append((path, False, a, b))          # a = disc offset, b = length
        i += 1
    return out


def layout(tree: str):
    sysd = os.path.join(tree, "sys")
    boot = open(os.path.join(sysd, "boot.bin"), "rb").read()
    dol_off, fst_off, fst_size = be32(boot, 0x420), be32(boot, 0x424), be32(boot, 0x428)
    fst = open(os.path.join(sysd, "fst.bin"), "rb").read()
    if len(fst) != fst_size:
        raise SystemExit(f"fst.bin is {len(fst)} bytes, boot.bin says {fst_size}")
    pieces = [("sys/boot.bin", 0, len(boot)), ("sys/bi2.bin", 0x440, os.path.getsize(os.path.join(sysd, "bi2.bin"))),
              ("sys/apploader.img", 0x2440, os.path.getsize(os.path.join(sysd, "apploader.img"))),
              ("sys/main.dol", dol_off, os.path.getsize(os.path.join(sysd, "main.dol"))),
              ("sys/fst.bin", fst_off, fst_size)]
    problems = []
    entries = read_fst(fst)
    seen = set()
    for path, is_dir, a, b in entries:
        if is_dir:
            continue
        host = os.path.join(tree, "files", *path.split("/"))
        seen.add(os.path.normcase(os.path.normpath(host)))
        if not os.path.isfile(host):
            problems.append(f"FST entry without a file: {path}")
            continue
        if os.path.getsize(host) != b:
            problems.append(f"size differs: {path} is {os.path.getsize(host)} bytes, the FST says {b}")
        pieces.append(("files/" + path, a, b))
    for root, _, fs in os.walk(os.path.join(tree, "files")):
        for f in fs:
            p = os.path.normcase(os.path.normpath(os.path.join(root, f)))
            if p not in seen:
                problems.append(f"file without an FST entry: {os.path.relpath(p, tree)}")
    ordered = sorted(pieces, key=lambda p: p[1])
    for x, y in zip(ordered, ordered[1:]):
        if x[1] + x[2] > y[1]:
            problems.append(f"overlap: {x[0]} [{x[1]:#x}, {x[1] + x[2]:#x}) and {y[0]} at {y[1]:#x}")
    end = max(p[1] + p[2] for p in pieces)
    return pieces, entries, problems, end


def main(argv):
    if len(argv) < 2 or argv[0] not in ("check", "build"):
        print(__doc__)
        return 2
    pieces, entries, problems, end = layout(argv[1])
    files = sum(1 for e in entries if not e[1])
    print(f"{files} files, {len(entries) - files} directories in the FST; image end {end:#x} ({end / 2**20:.1f} MiB)")
    for p in problems[:50]:
        print("  PROBLEM:", p)
    if problems:
        print(f"{len(problems)} problems: nothing written")
        return 1
    if argv[0] == "check":
        print("consistent: every FST file exists with its size, no extent overlaps")
        return 0
    image = argv[2]
    if "--dry-run" in argv:
        print(f"dry run: would write {image} ({end} bytes) from {len(pieces)} pieces")
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(image)), exist_ok=True)
    tmp = image + ".partial"
    h = hashlib.sha256()
    with open(tmp, "wb") as out:
        pos = 0
        for name, off, size in sorted(pieces, key=lambda p: p[1]):
            if off > pos:
                gap = b"\0" * (off - pos)
                out.write(gap)
                h.update(gap)
                pos = off
            with open(os.path.join(argv[1], *name.split("/")), "rb") as src:
                left = size
                while left:
                    chunk = src.read(min(left, 1 << 24))
                    out.write(chunk)
                    h.update(chunk)
                    left -= len(chunk)
            pos += size
    os.replace(tmp, image)
    with open(image + ".sha256", "w") as f:
        f.write(h.hexdigest() + "\n")
    print(f"wrote {image}: {pos} bytes, sha256 {h.hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
