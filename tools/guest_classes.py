"""The game's C++ classes and their virtual methods, recovered from main.dol's RTTI records.

The PAL main.dol carries demangled class names in 8-byte records {const char* name, void* vtable} at
0x805E4500..0x805EF020: a record with vtable 0 is a base-class entry of the next record that has one. A vtable is
1-2 header words then a contiguous run of .text addresses: skip leading non-text words, take the maximal run.
A function is named Class::vfN when it appears in exactly one class's vtable; in several, it gets the list.

  guest_classes.py <main.dol> [--function 0x801D6434 ...]   prints the classes owning those functions
"""
from __future__ import annotations

import argparse
import struct
from collections import defaultdict

RTTI_BEGIN, RTTI_END = 0x805E4500, 0x805EF020


class Dol:
    def __init__(self, data: bytes):
        self.data = data
        self.sections = []  # (address, offset, size, is_text)
        for i in range(18):
            offset, address, size = (struct.unpack_from(">I", data, base + i * 4)[0] for base in (0, 0x48, 0x90))
            if size:
                self.sections.append((address, offset, size, i < 7))

    def word(self, address):
        for start, offset, size, _ in self.sections:
            if start <= address and address + 4 <= start + size:
                return struct.unpack_from(">I", self.data, offset + address - start)[0]
        return None

    def string(self, address, limit=256):
        out = bytearray()
        for start, offset, size, _ in self.sections:
            if start <= address < start + size:
                position = offset + address - start
                while len(out) < limit and position < offset + size and self.data[position]:
                    out.append(self.data[position])
                    position += 1
                return out.decode("latin-1")
        return None

    def is_text(self, address):
        return any(text and start <= address < start + size for start, _, size, text in self.sections)


def classes(dol: Dol):
    """{class name: {"vtable": address, "bases": [names], "methods": [function addresses in slot order]}}"""
    result, bases = {}, []
    for record in range(RTTI_BEGIN, RTTI_END, 8):
        name_ptr, vtable = dol.word(record), dol.word(record + 4)
        name = dol.string(name_ptr) if name_ptr else None
        if not name:
            bases = []
            continue
        if vtable == 0:
            bases.append(name)
            continue
        methods, slot, started = [], vtable, False
        for _ in range(512):
            word = dol.word(slot)
            if word is None:
                break
            if dol.is_text(word):
                started = True
                methods.append(word)
            elif started:
                break
            elif slot - vtable >= 64 * 4:  # header words (RTTI pointers, offsets) never run this long
                break
            slot += 4
        result[name] = {"vtable": vtable, "bases": bases, "methods": methods}
        bases = []
    return result


def method_owners(table):
    """{function address: [(class, slot index)]}"""
    owners = defaultdict(list)
    for name, info in table.items():
        for index, function in enumerate(info["methods"]):
            owners[function].append((name, index))
    return owners


def label(owners, function):
    found = owners.get(function)
    if not found:
        return None
    if len(found) == 1:
        return f"{found[0][0]}::vf{found[0][1]}"
    names = sorted({name for name, _ in found})
    return names[0] if len(names) == 1 else f"[{len(names)} classes] " + ", ".join(names[:3])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dol")
    parser.add_argument("--function", action="append", default=[])
    args = parser.parse_args()
    dol = Dol(open(args.dol, "rb").read())
    table = classes(dol)
    owners = method_owners(table)
    print(f"{len(table)} classes with vtables, {sum(len(v['methods']) for v in table.values())} slots, "
          f"{len(owners)} distinct methods")
    for text in args.function:
        print(text, label(owners, int(text, 0)) or "-")


if __name__ == "__main__":
    main()
