import argparse
import json
from pathlib import Path
import re
import sys
import warnings

import numpy as np
from PIL import Image


FORMATS = {".png": "PNG", ".ppm": "PPM"}
INVALID = (OSError, ValueError, SyntaxError, Image.DecompressionBombError,
           Image.DecompressionBombWarning)


def validate_pair(a, b):
    if a.shape != b.shape:
        raise ValueError(f"dimension/channel mismatch: {a.shape} != {b.shape}; alpha cannot be invented")
    if (a.ndim != 3 or a.shape[2] not in (3, 4) or min(a.shape[:2]) < 1
            or a.dtype != np.uint8 or b.dtype != np.uint8):
        raise ValueError("expected nonempty uint8 RGB or real-alpha RGBA arrays")


def error_stats(delta):
    if not delta.size:
        return None
    delta = delta.astype(np.float64)
    return {"mae": float(np.abs(delta).mean()),
            "rmse": float(np.sqrt(np.square(delta).mean())),
            "max": float(np.abs(delta).max())}


def channel_stats(delta):
    return {name: error_stats(delta[..., i]) for i, name in enumerate("RGB")}


def alpha_stats(delta):
    present = delta.shape[2] == 4
    return {"status": "present" if present else "absent",
            "reference_present": present, "candidate_present": present,
            "metrics": error_stats(delta[..., 3]) if present else None}


def box_mean(a, window):
    sums = np.pad(a, ((1, 0), (1, 0), (0, 0))).cumsum(0).cumsum(1)
    return (sums[window:, window:] - sums[:-window, window:]
            - sums[window:, :-window] + sums[:-window, :-window]) / (window * window)


def local_ssim(a, b):
    validate_pair(a, b)
    window = min(7, a.shape[0], a.shape[1])
    a, b = a[..., :3].astype(np.float64), b[..., :3].astype(np.float64)
    ma, mb = box_mean(a, window), box_mean(b, window)
    va = np.maximum(box_mean(a * a, window) - ma * ma, 0.0)
    vb = np.maximum(box_mean(b * b, window) - mb * mb, 0.0)
    bound = np.sqrt(va * vb)
    covariance = np.clip(box_mean(a * b, window) - ma * mb, -bound, bound)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    scores = ((2 * ma * mb + c1) * (2 * covariance + c2)
              / ((ma * ma + mb * mb + c1) * (va + vb + c2)))
    channels = np.clip(scores, -1.0, 1.0).mean(axis=(0, 1))
    return {"method": "local box-window population SSIM, RGB channel mean; not Gaussian SSIM",
            "window": [window, window], "requested_window": [7, 7],
            "border": "valid", "stride": 1, "range": [0, 255], "L": 255,
            "K1": 0.01, "K2": 0.03,
            "roundoff": "nonnegative variances, covariance bounded by Cauchy-Schwarz, scores clipped to [-1,1]",
            "rgb": dict(zip("RGB", map(float, channels))), "mean": float(channels.mean())}


def compare_arrays(a, b):
    validate_pair(a, b)
    delta = b.astype(np.float64) - a.astype(np.float64)
    mask = np.any(a != b, axis=2)
    ys, xs = np.nonzero(mask)
    bbox = [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1] if xs.size else None
    return {"exact_match": not bool(mask.any()), "size": [a.shape[1], a.shape[0]],
            "mismatch_pixels": int(mask.sum()), "bbox_xyxy_exclusive": bbox,
            "rgb": channel_stats(delta), "alpha": alpha_stats(delta),
            "ssim": local_ssim(a, b),
            "edge_gradient_error": {
                "method": "RGB signed forward differences, candidate minus reference; no padding",
                "x": error_stats(np.diff(delta[..., :3], axis=1)),
                "y": error_stats(np.diff(delta[..., :3], axis=0)),
                "unavailable": "null axis metrics mean no adjacent pixels on that axis"}}


def temporal_error(previous_a, a, previous_b, b):
    validate_pair(a, b)
    validate_pair(previous_a, previous_b)
    validate_pair(previous_a, a)
    delta = ((b.astype(np.float64) - previous_b.astype(np.float64))
             - (a.astype(np.float64) - previous_a.astype(np.float64)))
    return {"rgb": channel_stats(delta), "alpha": alpha_stats(delta)}


def load_image(path):
    if path.suffix.lower() not in FORMATS or not path.is_file() or path.is_symlink():
        raise ValueError(f"unsupported image path (regular PNG/PPM required): {path}")
    with path.open("rb") as stream:
        header = stream.read(32 if path.suffix.lower() == ".png" else 65536)
    if path.suffix.lower() == ".png":
        if (len(header) < 29 or header[:8] != b"\x89PNG\r\n\x1a\n"
                or header[12:16] != b"IHDR" or header[24] not in (1, 2, 4, 8)):
            raise ValueError(f"unsupported PNG header/bit depth (at most 8 bits): {path}")
    else:
        tokens = re.findall(rb"[^\s#]+", re.sub(rb"#[^\r\n]*", b"", header))[:4]
        if len(tokens) != 4 or tokens[0] not in (b"P3", b"P6") or tokens[3] != b"255":
            raise ValueError(f"unsupported PPM header (P3/P6, maxval 255 required): {path}")
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        with Image.open(path) as image:
            if (image.format != FORMATS[path.suffix.lower()] or getattr(image, "n_frames", 1) != 1
                    or image.mode not in ("1", "L", "LA", "P", "RGB", "RGBA")):
                raise ValueError(f"unsupported image format, mode or multiple frames: {path}")
            image.verify()
        with Image.open(path) as image:
            image.load()
            alpha = "A" in image.getbands() or "transparency" in image.info
            return np.array(image.convert("RGBA" if alpha else "RGB"), dtype=np.uint8)


def capture_files(directory):
    files, identities = {}, {}
    for path in directory.iterdir():
        if not path.is_file() or path.is_symlink() or path.suffix.lower() not in FORMATS:
            raise ValueError(f"unsupported capture entry: {path}")
        match = re.search(r"([0-9]+)$", path.stem)
        if not match:
            raise ValueError(f"frame filename requires a trailing numeric identity: {path.name}")
        identity = int(match[1])
        if identity in identities:
            raise ValueError(f"duplicate numeric frame identity {identity}: {identities[identity]}, {path.name}")
        identities[identity] = path.name
        files[path.name] = (identity, path)
    if not files:
        raise ValueError(f"empty capture: {directory}")
    return files


def matched_paths(reference, candidate):
    if reference.is_symlink() or candidate.is_symlink():
        raise ValueError("symlink capture roots are unsupported")
    if reference.is_file() and candidate.is_file():
        return [(None, reference, candidate)], "explicit image pair"
    if not reference.is_dir() or not candidate.is_dir():
        raise ValueError("inputs must be two existing images or two existing frame directories")
    left, right = capture_files(reference), capture_files(candidate)
    if left.keys() != right.keys():
        raise ValueError(f"unmatched frame names; missing candidate: {sorted(left.keys() - right.keys())}; "
                         f"missing reference: {sorted(right.keys() - left.keys())}")
    return [(identity, path, right[name][1]) for name, (identity, path) in
            sorted(left.items(), key=lambda item: item[1][0])], "exact filenames, ascending trailing numeric ID"


def save_diffs(directory, name, a, b):
    difference = np.abs(b.astype(np.int16) - a.astype(np.int16)).astype(np.uint8)
    images = {"rgb_absolute": difference[..., :3],
              "mismatch_mask": np.any(a != b, axis=2).astype(np.uint8) * 255}
    if a.shape[2] == 4:
        images["alpha_absolute"] = difference[..., 3]
    paths = {}
    for kind, pixels in images.items():
        path = directory / f"{name}.{kind}.png"
        with path.open("xb") as stream:
            Image.fromarray(pixels).save(stream, format="PNG")
        paths[kind] = str(path)
    return paths


def compare_captures(reference, candidate, diff_dir=None):
    pairs, ordering = matched_paths(reference, candidate)
    if diff_dir is not None:
        diff_dir.mkdir()
    frames, transitions, previous = [], [], None
    for identity, left, right in pairs:
        a, b = load_image(left), load_image(right)
        result = compare_arrays(a, b)
        result.update({"reference": str(left), "candidate": str(right), "frame_id": identity})
        if previous is not None:
            previous_id, previous_name, previous_a, previous_b = previous
            transition = temporal_error(previous_a, a, previous_b, b)
            transition.update({"from": previous_name, "to": left.name,
                               "frame_id_delta": identity - previous_id})
            transitions.append(transition)
        if diff_dir is not None:
            result["diagnostic_images"] = save_diffs(diff_dir, left.name, a, b)
        frames.append(result)
        previous = identity, left.name, a, b
    exact = all(frame["exact_match"] for frame in frames)
    return {"status": "match" if exact else "mismatch", "exact_match": exact,
            "pairing": ordering, "frame_count": len(frames), "frames": frames,
            "temporal": {"status": "measured" if transitions else "unavailable_single_frame",
                         "method": "signed adjacent supplied-frame deltas, candidate minus reference; no time normalization",
                         "coverage": "adjacent supplied IDs need not be consecutive game frames",
                         "transitions": transitions}}


def check_destinations(args):
    for path in (args.output, args.diff_dir):
        if path is None:
            continue
        if path.exists() or path.is_symlink():
            raise ValueError(f"refusing existing output destination: {path}")
        if not path.parent.is_dir():
            raise ValueError(f"output parent directory must exist: {path.parent}")
        for source in (args.reference, args.candidate):
            if source.is_dir() and path.resolve().is_relative_to(source.resolve()):
                raise ValueError(f"output must be outside capture directories: {path}")
    if args.diff_dir is not None and args.output.resolve() == args.diff_dir.resolve():
        raise ValueError("JSON output and diagnostic directory must be different paths")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Strict local PNG/PPM decoded-pixel comparison; no Dolphin parity verdict.")
    parser.add_argument("reference", type=Path, help="image or directory of frames with trailing numeric IDs")
    parser.add_argument("candidate", type=Path, help="image or directory with exactly matching frame filenames")
    parser.add_argument("--output", required=True, type=Path, help="new JSON file; parent must exist, outside captures")
    parser.add_argument("--diff-dir", type=Path, help="new diagnostic directory; parent must exist, outside captures")
    args = parser.parse_args(argv)
    report = {"schema_version": 1, "scope": "local_decoded_pixels_only",
              "dolphin_reference_status": "not_verified_by_tool", "dolphin_parity": "not_assessed",
              "coverage": "all supplied frames; external capture completeness/provenance not verified",
              "policy": "exact decoded RGB and real alpha identity only; metrics are diagnostics",
              "decoding": "Pillow RGB/RGBA expansion; no resizing, alignment, orientation or color-profile transforms",
              "reference": str(args.reference), "candidate": str(args.candidate),
              "status": "invalid_capture", "exact_match": None}
    try:
        check_destinations(args)
    except INVALID as error:
        print(f"invalid capture/output: {error}", file=sys.stderr)
        return 2
    try:
        report.update(compare_captures(args.reference, args.candidate, args.diff_dir))
    except INVALID as error:
        report["error"] = str(error)
    try:
        payload = json.dumps(report, indent=2, allow_nan=False) + "\n"
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(payload)
    except INVALID as error:
        print(f"cannot write report: {error}", file=sys.stderr)
        return 2
    print(f"local comparison: {report['status']}; Dolphin parity not assessed; report: {args.output}")
    if "error" in report:
        print(report["error"], file=sys.stderr)
    return {"match": 0, "mismatch": 1, "invalid_capture": 2}[report["status"]]


if __name__ == "__main__":
    sys.exit(main())
