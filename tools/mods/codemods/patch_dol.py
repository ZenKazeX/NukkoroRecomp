#!/usr/bin/env python3
"""Write a patched COPY of a main.dol (in-place word changes, same size, same section header).

  patch_dol.py --dol <retail PAL main.dol> --out <copy> \
      --set 0x80498940=60000000                    one 32-bit word at a PAL address
      --words 0x80003EA4=38600005,4E800020         consecutive words (a cave function)
      --copy-word <other.dol>:0x80083338:0x8008347C  word of another DOL (NTSC address) written to a PAL address

The input is never modified (the output must be a different path). Addresses must be inside a file-backed text or data
section. This is the minimal producer of a variant DOL for SHADOW_RECOMP_DOL / SHADOW_RECOMP_VARIANT_OF
(docs/mods/codemods/RECOMPILE.md); no byte of any DOL is stored in the repository.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path


def sections(raw):
    h = struct.unpack(">18I18I18I", raw[:0xD8])
    return [(h[i], h[18 + i], h[36 + i]) for i in range(18) if h[36 + i]]


def offset(secs, addr):
    for off, ad, sz in secs:
        if ad <= addr < ad + sz:
            return off + addr - ad
    raise SystemExit(f"0x{addr:08X} is not inside a file-backed DOL section")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--words", action="append", default=[])
    ap.add_argument("--copy-word", action="append", default=[])
    a = ap.parse_args()
    if Path(a.out).resolve() == Path(a.dol).resolve():
        raise SystemExit("refusing to overwrite the input DOL")
    raw = bytearray(Path(a.dol).read_bytes())
    secs = sections(bytes(raw))
    changes = []

    def put(addr, word):
        o = offset(secs, addr)
        changes.append((addr, struct.unpack(">I", raw[o:o + 4])[0], word))
        raw[o:o + 4] = struct.pack(">I", word)

    for s in a.set:
        ad, w = s.split("=")
        put(int(ad, 16), int(w, 16))
    for s in a.words:
        ad, ws = s.split("=")
        for k, w in enumerate(ws.split(",")):
            put(int(ad, 16) + 4 * k, int(w, 16))
    for s in a.copy_word:
        # the path may hold a drive colon: split from the right
        path, na, pa = s.rsplit(":", 2)
        other = Path(path).read_bytes()
        o = offset(sections(other), int(na, 16))
        put(int(pa, 16), struct.unpack(">I", other[o:o + 4])[0])
    Path(a.out).write_bytes(bytes(raw))
    for ad, old, new in changes:
        print(f"0x{ad:08X}  {old:08X} -> {new:08X}")
    print(f"{a.out}: {len(changes)} word(s) changed, sha1 {hashlib.sha1(raw).hexdigest()}")


if __name__ == "__main__":
    sys.exit(main())
