"""Offline symbol resolution of a sample_threads.py json from the image's link map (no PDB needed).

The game tree (SHADOW_BUILD_TESTS=OFF) links with a .map and no .pdb, and in-process dbghelp finds no symbols on this
Windows install, so sample_threads.py returns raw instruction addresses. This resolves them against the map's
"Publics by Value" and "Static symbols" tables (Rva+Base column) and undecorates the names with dbghelp, so the output
has the same shape as resolve.py's (names like gekko::recomp::fn_80012345_body) minus source lines.

usage: resolve_map.py <samples.json> <image.map> <runtime base hex> <out.json>
"""
import bisect
import collections
import ctypes
import json
import re
import sys

ROW = re.compile(r"^\s[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")


def load_map(path):
    """Sorted (address, decorated name) of every code or data symbol, and the preferred base."""
    base = 0x140000000
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
                continue
            match = ROW.match(line)
            if match:
                address = int(match.group(2), 16)
                if address >= base:
                    rows.append((address, match.group(1)))
    rows.sort()
    return base, rows


class Undecorator:
    def __init__(self):
        self.dbghelp = ctypes.WinDLL("dbghelp")
        self.dbghelp.UnDecorateSymbolName.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
        self.buffer = ctypes.create_string_buffer(2048)
        self.cache = {}

    def __call__(self, name):
        if name not in self.cache:
            plain = name
            if name.startswith("?"):
                # UNDNAME_NAME_ONLY: the qualified name without return type and arguments.
                if self.dbghelp.UnDecorateSymbolName(name.encode(), self.buffer, len(self.buffer), 0x1000):
                    plain = self.buffer.value.decode(errors="replace")
            self.cache[name] = plain
        return self.cache[name]


def main(argv):
    if len(argv) != 5:
        print(__doc__)
        return 2
    source, map_path, runtime_base, out = argv[1], argv[2], int(argv[3], 16), argv[4]
    base, rows = load_map(map_path)
    if not rows:
        print("no symbol rows in the map")
        return 2
    addresses = [row[0] for row in rows]
    limit = rows[-1][0] + 0x100000
    undecorate = Undecorator()
    data = json.load(open(source))
    unresolved = 0
    for thread in data["threads"].values():
        functions = collections.Counter()
        for name, count, _ in thread["top"]:
            if not name.startswith("0x"):
                functions[name] += count
                continue
            address = int(name, 16) - runtime_base + base
            if base <= address < limit:
                index = bisect.bisect_right(addresses, address) - 1
                functions[undecorate(rows[index][1])] += count
            else:
                functions[name] += count
                unresolved += count
        total = sum(functions.values())
        thread["top"] = [[f, c, round(c / total * 100, 2)] for f, c in functions.most_common()]
        thread["lines"] = []
    json.dump(data, open(out, "w"), indent=1)
    print(f"{len(rows)} map symbols; {unresolved} samples outside the image")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
