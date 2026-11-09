#!/usr/bin/env python3
"""Scale-fit acceptance for the 2D widescreen layout.

The host maps 2D content by  x' = A + s*(x - A)  in a 640x480 virtual canvas,
where s = (4:3)/target_aspect and A is the group's anchor. A centred screen is
therefore the 4:3 frame warped about canvas 320 by exactly s, and a screen that
received no correction at all is the 4:3 frame unchanged. Fitting each frame
pair against both hypotheses is what catches a screen silently left in 4:3 - an
error that is invisible in a single capture because the frame still looks like
the game.

    python scale-fit.py <ref-4x3-dir> <wide-dir> [--aspect 16:9] [--report csv]

Both directories are --filmstrip captures of the SAME route and stride, so the
frame indices line up: guest execution is byte-identical across aspects.
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image

ASPECTS = {
    "4:3": 4.0 / 3.0,
    "16:9": 16.0 / 9.0,
    "16:10": 16.0 / 10.0,
    "21:9": 21.0 / 9.0,
    "32:9": 32.0 / 9.0,
}


def load(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.float32)


def predict(reference, scale):
    """The wide frame the host should have produced from this 4:3 frame."""
    height, width = reference.shape
    centre = width * 0.5
    xs = centre + (np.arange(width) - centre) / scale
    x0 = np.clip(np.floor(xs).astype(int), 0, width - 1)
    x1 = np.clip(x0 + 1, 0, width - 1)
    t = np.clip(xs - np.floor(xs), 0.0, 1.0)
    return reference[:, x0] * (1.0 - t) + reference[:, x1] * t


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("wide")
    parser.add_argument("--aspect", default="16:9")
    parser.add_argument("--min-contrast", type=float, default=4.0,
                        help="skip frames too flat to carry scale information")
    parser.add_argument("--report")
    args = parser.parse_args()

    if args.aspect not in ASPECTS:
        sys.exit("unknown aspect %s (known: %s)" % (args.aspect, ", ".join(ASPECTS)))
    expected = round((4.0 / 3.0) / ASPECTS[args.aspect], 6)
    candidates = sorted({expected, 1.0})

    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(args.reference, "*.ppm")))
    rows, uncorrected, compared = [], [], 0
    for name in names:
        a_path = os.path.join(args.reference, name)
        b_path = os.path.join(args.wide, name)
        if not os.path.exists(b_path):
            continue
        a, b = load(a_path), load(b_path)
        if a.shape != b.shape or float(a.std()) < args.min_contrast:
            continue
        compared += 1
        # Compare only the band the prediction can actually source from. A
        # column at x' reads the reference at 320 + (x'-320)/s, so only
        # |x'-320| <= 320*s lands inside the 4:3 frame; outside it every
        # hypothesis is comparing clamped edge pixels and the fit is noise.
        # Take the narrower of the two candidate bands so both are scored on
        # the same pixels.
        half = int(a.shape[1] * 0.5 * min(candidates)) - 2
        margin = max(1, a.shape[1] // 2 - half)
        scores = {s: float(np.abs(predict(a, s) - b)[:, margin:-margin].mean())
                  for s in candidates}
        best = min(scores, key=scores.get)
        rows.append((name, best, scores, float(a.std())))
        # Only a DECISIVE win for scale 1.0 is a finding. On the last frames of
        # a fade the picture is nearly uniform and every hypothesis fits it
        # about as well, so a bare argmin there reports noise as a defect.
        if abs(best - expected) > 1e-6 and scores[1.0] < scores[expected] * 0.7:
            uncorrected.append((name, scores, float(a.std())))

    print("aspect %s, expected scale %.4f" % (args.aspect, expected))
    print("%d frame pairs compared, %d received NO correction" % (compared, len(uncorrected)))
    for name, scores, contrast in uncorrected:
        print("  %s  %s  contrast %.1f" %
              (name, "  ".join("d(%.4f)=%.3f" % (s, d) for s, d in sorted(scores.items())), contrast))
    if args.report:
        with open(args.report, "w", encoding="utf-8") as handle:
            handle.write("frame,best_scale,contrast," +
                         ",".join("d_%.4f" % s for s in candidates) + "\n")
            for name, best, scores, contrast in rows:
                handle.write("%s,%.4f,%.2f,%s\n" %
                             (name, best, contrast,
                              ",".join("%.4f" % scores[s] for s in candidates)))
        print("wrote %s" % args.report)
    return 1 if uncorrected else 0


if __name__ == "__main__":
    sys.exit(main())
