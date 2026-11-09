"""Rank sampled game-thread stacks by the game's own classes (OPTIMIZATION_PLAN A3).

Input: stacks.folded from `native_sample.py --stacks` (one "thread;outer;...;inner count" line per distinct stack;
generated frames are named fn_XXXXXXXX by guest address) and main.dol for the RTTI class map (guest_classes.py).
Each sample is charged to:
  - task: the OUTERMOST guest frame that is a class's virtual method - the object the scheduler is running;
  - owner: the INNERMOST such frame - the object whose method is doing the work (helpers below it are its work);
  - inclusive classes: every class with a method anywhere on the stack, once per sample;
and guest functions are ranked inclusively with their nearest owning class. Samples with no class method on the
stack are "(no class)", and samples outside generated code (runtime, waits) keep their innermost non-guest frame.

  stack_report.py --folded <run>/stacks.folded --dol main.dol [--thread-name-filter ...] --out report.json
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

import guest_classes


def parse(path):
    for line in Path(path).read_text().splitlines():
        stack, _, count = line.rpartition(" ")
        frames = stack.split(";")
        yield frames[0], frames[1:], int(count)


def guest(frame):
    if frame.startswith("fn_") and len(frame) == 11:
        try:
            return int(frame[3:], 16)
        except ValueError:
            return None
    return None


def rank(counter, total, limit):
    return [{"name": name, "samples": n, "share": round(n / total, 4)} for name, n in counter.most_common(limit)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--folded", required=True, action="append", help="repeat to merge runs")
    parser.add_argument("--dol", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--limit", type=int, default=60)
    args = parser.parse_args()
    dol = guest_classes.Dol(Path(args.dol).read_bytes())
    owners = guest_classes.method_owners(guest_classes.classes(dol))

    def class_of(frame):
        address = guest(frame)
        return guest_classes.label(owners, address) if address is not None else None

    total = 0
    task, owner, classes, functions, function_owner, outside = Counter(), Counter(), Counter(), Counter(), {}, Counter()
    in_guest = 0
    for _, frames, count in (item for path in args.folded for item in parse(path)):
        total += count
        labels = [class_of(f) for f in frames]
        named = [(f, l) for f, l in zip(frames, labels) if l]
        if any(guest(f) is not None for f in frames):
            in_guest += count
        task[named[0][1] if named else "(no class)"] += count
        owner[named[-1][1] if named else "(no class)"] += count
        for name in {l.split("::vf")[0] for l in labels if l}:
            classes[name] += count
        nearest = None
        seen = set()
        for frame, l in zip(frames, labels):
            nearest = l or nearest
            if guest(frame) is not None and frame not in seen:
                seen.add(frame)
                functions[frame] += count
                function_owner.setdefault(frame, Counter())[nearest or "(no class)"] += count
        if guest(frames[-1]) is None:
            outside[frames[-1]] += count
    if not total:
        raise SystemExit("no samples")
    result = {
        "samples": total, "samples_with_guest_frames": in_guest,
        "by_task": rank(task, total, args.limit),
        "by_owner": rank(owner, total, args.limit),
        "inclusive_by_class": [{"name": n.split("::vf")[0], "samples": c, "share": round(c / total, 4)}
                               for n, c in classes.most_common(args.limit)],
        "inclusive_guest_functions": [
            {"function": f, "samples": n, "share": round(n / total, 4),
             "under": function_owner[f].most_common(1)[0][0]} for f, n in functions.most_common(args.limit)],
        "innermost_outside_generated_code": rank(outside, total, 30),
        "limits": ["Wall-time samples: waits are included (see innermost_outside_generated_code).",
                   "Class names come from RTTI vtables; non-virtual functions are charged to the nearest enclosing "
                   "virtual method on the stack.",
                   "A method shared by several classes is labelled with the class list."],
    }
    Path(args.out).write_text(json.dumps(result, indent=2) + "\n")
    for key in ("by_task", "by_owner"):
        print(f"== {key}")
        for row in result[key][:20]:
            print(f"{row['samples']:6d} {row['share']:7.1%} {row['name']}")


if __name__ == "__main__":
    main()
