#!/usr/bin/env python3
"""Find HOST-SIDE 2D instability in a --hud-report capture.

A flicker the player sees as a 2D element jittering is, in this layer, a primitive whose
GUEST geometry is identical between two frames but whose MAPPED geometry is not. The guest
did not move it; the host did. Nothing else in the capture can distinguish that from the
guest animating something, which is why this check keys on the guest box being unchanged.

    python flicker-scan.py <hud-report.csv> [--from N] [--to N] [--tolerance 0.01]

Reports, per identity:
  * mapped-position changes with no guest-position change  -- host-side movement
  * anchor changes                                          -- reclassification
  * classification churn                                    -- how often either happened

Identity is (cast, group_id, guest box) where a cast is available, else (group_id, guest box).
The cast column comes from the Chao::CSD per-cast observation channel; rows with cast 0x0 are
not CSD (fades, glyph runs, the boot dialog) and are keyed on their guest box alone.
"""
import argparse
import collections
import csv
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report")
    parser.add_argument("--from", dest="first", type=int, default=0)
    parser.add_argument("--to", dest="last", type=int, default=10 ** 9)
    parser.add_argument("--tolerance", type=float, default=0.01,
                        help="canvas units below which a difference is rounding")
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()

    rows = [r for r in csv.DictReader(open(args.report, encoding="utf-8"))
            if args.first <= int(r["frame"]) <= args.last]
    if not rows:
        sys.exit("no rows in that frame range")
    has_cast = "cast" in rows[0]

    # identity -> frame -> (mapped_x0, mapped_x1, anchor)
    seen = collections.defaultdict(dict)
    for r in rows:
        guest = (round(float(r["x0"]), 3), round(float(r["x1"]), 3),
                 round(float(r["y0"]), 3), round(float(r["y1"]), 3))
        cast = r.get("cast", "0x0")
        key = (cast if has_cast and cast != "0x0" else "-", r["group_id"], guest)
        seen[key][int(r["frame"])] = (float(r["mapped_x0"]), float(r["mapped_x1"]), r["anchor"])

    moved, reclassified = [], []
    for key, frames in seen.items():
        if len(frames) < 2:
            continue
        order = sorted(frames)
        pos_changes = anchor_changes = 0
        span = 0.0
        for i in range(1, len(order)):
            a = frames[order[i - 1]]
            b = frames[order[i]]
            if abs(a[0] - b[0]) > args.tolerance or abs(a[1] - b[1]) > args.tolerance:
                pos_changes += 1
                span = max(span, abs(a[0] - b[0]), abs(a[1] - b[1]))
            if a[2] != b[2]:
                anchor_changes += 1
        if pos_changes:
            moved.append((pos_changes, span, len(order), key))
        if anchor_changes:
            reclassified.append((anchor_changes, len(order), key,
                                 collections.Counter(v[2] for v in frames.values())))

    print("%d rows, %d distinct (identity, guest box) combinations" % (len(rows), len(seen)))
    print()
    print("HOST-SIDE MOVEMENT - the guest box is unchanged but the mapped box is not: %d"
          % len(moved))
    for changes, span, n, key in sorted(moved, reverse=True)[:args.top]:
        print("   cast=%-11s group=%-11s guest x %8.2f..%-8.2f  %d changes over %d frames, "
              "max %.2f canvas units" % (key[0], key[1], key[2][0], key[2][1], changes, n, span))
    print()
    print("RECLASSIFICATION - the same guest box takes different anchors: %d" % len(reclassified))
    for changes, n, key, anchors in sorted(reclassified, reverse=True)[:args.top]:
        print("   cast=%-11s group=%-11s guest x %8.2f..%-8.2f  %d changes over %d frames  %s"
              % (key[0], key[1], key[2][0], key[2][1], changes, n, dict(anchors)))

    return 1 if (moved or reclassified) else 0


if __name__ == "__main__":
    sys.exit(main())
