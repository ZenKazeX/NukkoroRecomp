"""Name and attribute GEKKO_SELF_SAMPLE stacks (runtime/include/gekko/self_sampler.h). MEASUREMENT ONLY.

    self_sample_report.py --map ShadowTheHedgehog.map --samples s1.csv [--samples s2.csv ...]
                          [--frames fw1.csv ... --select 32 33] [--top 40]

Each sample line is `steady_ns,rva,rva,...` (innermost frame first; '@'-prefixed absolute addresses lie outside the
image). Frames are named by the nearest preceding public code symbol of the link map; generated guest functions are
shown by their guest name (fn_XXXXXXXX). With --frames (GEKKO_FRAME_WORK logs, one per --samples file, same order)
and --select, only samples taken inside those game frames are counted. Prints inclusive (once per sample per
function) and exclusive (innermost frame) shares, and the innermost frame inside generated guest code ("guest self").
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import re
import sys

SYMBOL = re.compile(r"^\s*0001:([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s")
LOAD = re.compile(r"Preferred load address is ([0-9a-fA-F]+)")
GUEST = re.compile(r"fn_([0-9a-fA-F]{8})")


def read_map(path):
    base = None
    starts, names = [], []
    with open(path, errors="replace") as handle:
        for line in handle:
            if base is None:
                found = LOAD.search(line)
                if found:
                    base = int(found[1], 16)
                continue
            match = SYMBOL.match(line)
            if not match:
                continue
            rva = int(match[3], 16) - base
            starts.append(rva)
            names.append(match[2])
    order = sorted(range(len(starts)), key=starts.__getitem__)
    return [starts[i] for i in order], [names[i] for i in order]


def short(name):
    guest = GUEST.search(name)
    if guest:
        return "fn_" + guest[1].upper()
    # ?name@scope@@... -> scope::name
    if name.startswith("?"):
        parts = name[1:].split("@@", 1)[0].split("@")
        return "::".join(reversed([p for p in parts if p]))
    return name


class Namer:
    def __init__(self, path):
        self.starts, self.names = read_map(path)
        self.cache = {}

    def __call__(self, token):
        if token.startswith("@"):
            return "(outside the image)"
        rva = int(token, 16)
        found = self.cache.get(rva)
        if found is None:
            at = bisect.bisect_right(self.starts, rva) - 1
            found = short(self.names[at]) if at >= 0 else "(before the first symbol)"
            self.cache[rva] = found
        return found


def frame_windows(path, select):
    rows = {int(r["frame"]): int(r["host_ns"]) for r in csv.DictReader(open(path))}
    return [(rows[f], rows[f + 1]) for f in select if f in rows and f + 1 in rows]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", required=True)
    parser.add_argument("--samples", action="append", required=True)
    parser.add_argument("--frames", action="append", default=[])
    parser.add_argument("--select", type=int, nargs="*", default=[])
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args(argv)
    if args.frames and len(args.frames) != len(args.samples):
        parser.error("one --frames log per --samples file")
    name = Namer(args.map)
    inclusive, exclusive, guest_self = collections.Counter(), collections.Counter(), collections.Counter()
    total = 0
    for index, path in enumerate(args.samples):
        windows = frame_windows(args.frames[index], args.select) if args.frames and args.select else None
        with open(path) as handle:
            for line in handle:
                if line.startswith("#") or not line.strip():
                    continue
                fields = line.rstrip("\n").split(",")
                ns = int(fields[0])
                if windows is not None and not any(lo <= ns < hi for lo, hi in windows):
                    continue
                frames = [name(token) for token in fields[1:]]
                if not frames:
                    continue
                total += 1
                exclusive[frames[0]] += 1
                for f in set(frames):
                    inclusive[f] += 1
                guest = next((f for f in frames if f.startswith("fn_")), None)
                if guest:
                    guest_self[guest] += 1
    print(f"{total} samples")
    for title, table in (("inclusive", inclusive), ("exclusive (innermost)", exclusive),
                         ("innermost guest function", guest_self)):
        print(f"-- {title}")
        for f, n in table.most_common(args.top):
            print(f"  {100.0 * n / max(total, 1):5.1f}%  {n:6d}  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
