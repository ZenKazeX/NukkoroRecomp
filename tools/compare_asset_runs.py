"""Compare control/A/B captures. --partial is diagnostic, never a final verdict."""
import argparse
import hashlib
import json
from pathlib import Path


def trace(path, partial):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            if partial:
                continue
            raise
        # "file_read" from the native file API; "di_read" in archived traces
        # from the DI device model. Same fields, same meaning, for this reader.
        if item.get("ev") in ("file_read", "di_read"):
            rows.append(tuple(item.get(k) for k in (
                "file", "file_offset", "length", "bytes_fnv1a", "dest_guest")))
    return rows


def digest(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "md5").hexdigest()


def compare(root, a, b, partial):
    left, right = (trace(root / f"{arm}-trace.jsonl", partial) for arm in (a, b))
    differences = [(i, x, y) for i, (x, y) in enumerate(zip(left, right)) if x != y]
    print(f"{a}/{b}: DI counts {len(left)}/{len(right)}, aligned differences {len(differences)}")
    for row in differences[:3]:
        print("  DI", row)
    frames = [dict((p.name, p) for p in (root / f"{arm}-film").glob("*.ppm")) for arm in (a, b)]
    common = sorted(frames[0].keys() & frames[1].keys())
    changed = [name for name in common if digest(frames[0][name]) != digest(frames[1][name])]
    print(f"  frames {len(frames[0])}/{len(frames[1])}, common {len(common)}, differ {len(changed)}; first {changed[:5]}")
    first = None
    with (root / f"{a}-audio.pcm").open("rb") as f, (root / f"{b}-audio.pcm").open("rb") as g:
        offset = 0
        while True:
            x, y = f.read(65536), g.read(65536)
            if not x or not y:
                break
            count = min(len(x), len(y))
            if x[:count] != y[:count] and first is None:
                first = offset + next(i for i in range(count) if x[i] != y[i])
            offset += count
    sizes = [(root / f"{arm}-audio.pcm").stat().st_size for arm in (a, b)]
    print(f"  PCM bytes {sizes}, aligned {offset}, first difference {first}")
    return not differences and not changed and first is None and (partial or (
        len(left) == len(right) and frames[0].keys() == frames[1].keys() and sizes[0] == sizes[1]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("arms", nargs="+", help="reference followed by comparison arms")
    parser.add_argument("--partial", action="store_true")
    args = parser.parse_args()
    print("PROVISIONAL ALIGNED PREFIX ONLY" if args.partial else "COMPLETED CAPTURE COMPARISON")
    verdict = True
    for arm in args.arms[1:]:
        verdict &= compare(args.root, args.arms[0], arm, args.partial)
    raise SystemExit(0 if verdict else 1)
