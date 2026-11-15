#!/usr/bin/env python3
"""Write a copy of extract/functions.csv with extra function rows (new code: caves, appended routines).

  add_function_rows.py --functions extract/functions.csv --out <copy.csv> --row 0x80003EA4:8 [--row ADDR:BYTES ...]

The recompiler only translates what the function table lists (a row per entry point; rows may overlap, the table
already nests entry points inside their function). A cave reached by `b`/`bl` from a patched site but absent from
the table is not translated: the variant builds, then stops at run time with "native execution cannot continue at
<cave> ... PPC interpretation is forbidden" (docs/mods/codemods/RECOMPILE.md, experiment B).
Build with -DSHADOW_RECOMP_FUNCTIONS=<copy.csv>. The repository's table is never edited.
"""
import argparse
import csv
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--row", action="append", required=True, help="ADDR:BYTES of a new function")
    a = ap.parse_args()
    if a.out == a.functions:
        raise SystemExit("refusing to overwrite the input table")
    with open(a.functions, encoding="utf-8", newline="") as f:
        rd = csv.reader(f)
        header = next(rd)
        rows = list(rd)
    have = {int(r[1]) for r in rows}
    cols = {n: i for i, n in enumerate(header)}
    for spec in a.row:
        addr_s, size_s = spec.split(":")
        addr, size = int(addr_s, 16), int(size_s, 0)
        if addr % 4 or size % 4 or size < 4:
            raise SystemExit(f"{spec}: address and size must be word multiples")
        if addr in have:
            raise SystemExit(f"{spec}: a row already starts there")
        r = [""] * len(header)
        r[cols["addr_hex"]] = f"0x{addr:08x}"
        r[cols["addr"]] = str(addr)
        r[cols["name"]] = f"cave_{addr:08x}"
        r[cols["size"]] = str(size)
        r[cols["ins"]] = str(size // 4)
        rows.append(r)
    rows.sort(key=lambda r: int(r[1]))
    with open(a.out, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, quoting=csv.QUOTE_NONNUMERIC if False else csv.QUOTE_MINIMAL, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)
    print(f"{a.out}: {len(rows)} rows ({len(a.row)} added)")


if __name__ == "__main__":
    sys.exit(main())
