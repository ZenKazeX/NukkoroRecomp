#!/usr/bin/env python3
"""Turn a --hud-report capture into a per-screen, per-cast decomposition table.

The `cast` column names the authored Chao::CSD cast that drew each primitive (see
2D_UI_DECOMPOSITION_AND_WIDESCREEN_MAP.md section 2.1). Cast pointers of one loaded .gncp are
contiguous on a 0x50 stride, so clustering them recovers "which asset" without needing the
guest to say so, and the offset within a cluster is the authored cast index.

    python cast-map.py <hud-report.csv> [--gap 0x4000] [--min-frames 20] [--top 40]

For each cluster it prints the frames it was live, and one row per cast: the authored index, how
many frames it drew on, its guest canvas box, the anchors it took, and a suggested policy. The
suggestion is advisory only - it is what the geometry allows, not what the composition wants.
"""
import argparse
import collections
import csv
import sys


def suggest(x0, x1, y0, y1, anchors, textured):
    reaches_left = x0 <= 8.0
    reaches_right = x1 >= 632.0
    if reaches_left and reaches_right:
        return "FULLSCREEN" if (y1 - y0) >= 432.0 else "FULL_WIDTH"
    if x1 <= 8.0 or x0 >= 632.0:
        return "OFF_CANVAS"
    if x1 <= 160.0:
        return "LEFT_ANCHORED?"
    if x0 >= 480.0:
        return "RIGHT_ANCHORED?"
    return "CENTERED"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report")
    parser.add_argument("--gap", type=lambda v: int(v, 0), default=0x4000,
                        help="cast-pointer gap that starts a new asset cluster")
    parser.add_argument("--min-frames", type=int, default=20)
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()

    rows = list(csv.DictReader(open(args.report, encoding="utf-8")))
    if not rows or "cast" not in rows[0]:
        sys.exit("this capture has no 'cast' column - rebuild and re-capture")

    casts = sorted({int(r["cast"], 16) for r in rows if r["cast"] != "0x0"})
    if not casts:
        sys.exit("no CSD casts in this capture")

    # Cluster contiguous cast pointers: one cluster == one loaded .gncp.
    clusters, current = [], [casts[0]]
    for c in casts[1:]:
        if c - current[-1] > args.gap:
            clusters.append(current)
            current = []
        current.append(c)
    clusters.append(current)

    by_cast = collections.defaultdict(list)
    for r in rows:
        if r["cast"] != "0x0":
            by_cast[int(r["cast"], 16)].append(r)

    print("%d rows, %d distinct casts, %d asset clusters\n" % (len(rows), len(casts), len(clusters)))

    for cluster in clusters:
        frames = set()
        for c in cluster:
            frames.update(int(r["frame"]) for r in by_cast[c])
        if len(frames) < args.min_frames:
            continue
        base = cluster[0]
        print("=" * 100)
        print("ASSET CLUSTER base 0x%08x   %d casts   live on frames %d..%d (%d frames)"
              % (base, len(cluster), min(frames), max(frames), len(frames)))
        print("  %-6s %-11s %-6s %-34s %-24s %s"
              % ("idx", "cast", "frames", "guest canvas box", "anchors", "allows"))
        for c in cluster[:args.top]:
            rs = by_cast[c]
            x0 = min(float(r["x0"]) for r in rs)
            x1 = max(float(r["x1"]) for r in rs)
            y0 = min(float(r["y0"]) for r in rs)
            y1 = max(float(r["y1"]) for r in rs)
            anchors = collections.Counter(r["anchor"] for r in rs)
            textured = any(r["textured"] == "1" for r in rs)
            idx = (c - base) // 0x50
            print("  %-6d 0x%08x %-6d x %7.1f..%-7.1f y %7.1f..%-7.1f  %-24s %s"
                  % (idx, c, len({int(r["frame"]) for r in rs}), x0, x1, y0, y1,
                     ",".join("%s:%d" % kv for kv in anchors.most_common(3)),
                     suggest(x0, x1, y0, y1, anchors, textured)))
        if len(cluster) > args.top:
            print("  ... %d more casts" % (len(cluster) - args.top))
        print()


if __name__ == "__main__":
    main()
