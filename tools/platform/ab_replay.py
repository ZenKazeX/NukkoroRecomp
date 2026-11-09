"""Run one recorded replay through several runtime backends and compare what the game did.

    python -B ab_replay.py --exe <ShadowTheHedgehog.exe> --recording <recording dir> --out <dir>
                           [--arm NAME=ARGS ...] [--budget-ms N] [--stage stgNNNN]

Each arm runs the same executable on fresh copies of the recording's initial/ files (card, SRAM, display.cfg,
pad.cfg) and its input.tape, headless, without audio and without XInput (nothing reaches the user's devices), with
--state-report (the end-of-run report with a digest of every 64 KiB block of guest memory) and --json-report. Arms
are given as NAME=extra arguments, e.g. host=--storage-backend host-directory. The first arm is the reference;
every other arm must reach the same game tick and VI frame count with every memory block digest equal, and the same
asset reads and bytes. Differences are listed block by block (region, offset). Writes <out>/summary.json.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import time


def run_arm(exe, recording, out, name, extra, budget_ms, stage, game_data):
    d = os.path.join(out, name)
    if os.path.exists(d):
        shutil.rmtree(d)
    os.makedirs(d)
    init = os.path.join(recording, "initial")
    for f in ("card.raw", "SRAM.pc.raw", "display.cfg", "pad.cfg"):
        shutil.copyfile(os.path.join(init, f), os.path.join(d, f))
    shutil.copyfile(os.path.join(recording, "input.tape"), os.path.join(d, "input.tape"))
    argv = [exe, "--game-data", game_data, "--headless", "--no-audio", "--memory-card", "card.raw",
            "--display-cfg", "display.cfg", "--pad-cfg", "pad.cfg", "--state-file", "unused.gekkostate",
            "--snapshot-key", "none", "--no-xinput", "--boot-stage", stage, "--budget-ms", str(budget_ms),
            "--input-tape-play", "input.tape", "--native-output", "1280x720", "--width", "1280", "--height", "720",
            "--json-report", "session.json", "--state-report", "state.json"] + extra
    with open(os.path.join(d, "invocation.json"), "w") as f:
        json.dump({"argv": argv}, f, indent=1)
    started = time.monotonic()
    with open(os.path.join(d, "stdout.log"), "wb") as so, open(os.path.join(d, "stderr.log"), "wb") as se:
        p = subprocess.run(argv, cwd=d, stdout=so, stderr=se, stdin=subprocess.DEVNULL, timeout=1800)
    wall = time.monotonic() - started
    state = json.load(open(os.path.join(d, "state.json"))) if os.path.exists(os.path.join(d, "state.json")) else None
    session = json.load(open(os.path.join(d, "session.json"))) if os.path.exists(os.path.join(d, "session.json")) else None
    stdout = open(os.path.join(d, "stdout.log"), encoding="utf-8", errors="replace").read()
    return {"name": name, "exit": p.returncode, "wall_seconds": round(wall, 1), "state": state, "session": session,
            "backend_line": next((l for l in stdout.splitlines() if l.startswith("Storage backend:")), None),
            "tape_complete": "[input-tape] played to the end" in stdout}


def compare(ref, other):
    diffs = []
    a, b = ref["state"], other["state"]
    if a is None or b is None:
        return ["a state report is missing"]
    for k in ("game_ticks", "vi_frames", "boundary_tick"):
        if a.get(k) != b.get(k):
            diffs.append(f"{k}: {a.get(k)} vs {b.get(k)}")
    ra = {r["name"]: r for r in a.get("memory", {}).get("regions", [])}
    rb = {r["name"]: r for r in b.get("memory", {}).get("regions", [])}
    if set(ra) != set(rb):
        diffs.append(f"memory regions differ: {sorted(ra)} vs {sorted(rb)}")
    blocks = 0
    for n in sorted(set(ra) & set(rb)):
        x, y = ra[n], rb[n]
        if x["committed"] != y["committed"]:
            diffs.append(f"{n}: committed {x['committed']} vs {y['committed']}")
        for i, (h1, h2) in enumerate(zip(x["blocks"], y["blocks"])):
            blocks += 1
            if h1 != h2:
                diffs.append(f"{n} block {i} (offset {i * 65536:#x}): {h1} vs {h2}")
    for k in ("reads", "bytes", "unmapped", "out_of_range", "failed"):
        sa, sb = (ref["session"] or {}).get("assets", {}).get(k), (other["session"] or {}).get("assets", {}).get(k)
        if sa != sb:
            diffs.append(f"assets.{k}: {sa} vs {sb}")
    return diffs, blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--recording", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--game-data", default="H:/ShadowTheHedgehogDiskGC_Extract/game")
    ap.add_argument("--arm", action="append", required=True, help="NAME=extra arguments")
    ap.add_argument("--budget-ms", type=int, default=316000)
    ap.add_argument("--stage", default="stg0100")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    runs = []
    for spec in args.arm:
        name, _, extra = spec.partition("=")
        r = run_arm(os.path.abspath(args.exe), os.path.abspath(args.recording), os.path.abspath(args.out), name,
                    shlex.split(extra, posix=False), args.budget_ms, args.stage, args.game_data)
        print(f"{name}: exit {r['exit']}, {r['wall_seconds']} s, tape complete {r['tape_complete']}, "
              f"{r['backend_line']}")
        runs.append(r)
    ref = runs[0]
    results = []
    ok = ref["exit"] == 0 and ref["tape_complete"]
    for r in runs[1:]:
        diffs, blocks = compare(ref, r)
        same = not diffs and r["exit"] == 0 and r["tape_complete"]
        ok = ok and same
        results.append({"arm": r["name"], "against": ref["name"], "identical": same, "blocks_compared": blocks,
                        "differences": diffs[:200]})
        print(f"{r['name']} vs {ref['name']}: {'IDENTICAL' if same else 'DIFFERENT'} ({blocks} memory blocks compared, "
              f"{len(diffs)} differences)")
        for d in diffs[:10]:
            print("   ", d)
    summary = {"exe": os.path.abspath(args.exe), "recording": args.recording, "budget_ms": args.budget_ms,
               "arms": [{k: v for k, v in r.items() if k not in ("state", "session")} |
                        {"game_ticks": (r["state"] or {}).get("game_ticks"), "vi_frames": (r["state"] or {}).get("vi_frames"),
                         "asset_reads": ((r["session"] or {}).get("assets") or {}).get("reads"),
                         "asset_bytes": ((r["session"] or {}).get("assets") or {}).get("bytes"),
                         "storage_backend": ((r["session"] or {}).get("assets") or {}).get("storage_backend")}
                        for r in runs],
               "comparisons": results, "all_identical": ok}
    json.dump(summary, open(os.path.join(args.out, "summary.json"), "w"), indent=1)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
