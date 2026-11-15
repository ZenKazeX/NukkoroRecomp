#!/usr/bin/env python3
"""In-game proof of the data-code engine (docs/mods/codemods/ENGINE.md section 5).

Runs the headless Westopolis tape (stg0100) to a fixed game tick and compares the captured picture:

  base-off   a stock executable (built from mods/loader), no codes
  cand-off   the engine executable, no codes                     -> must equal base-off (zero cost when off)
  on         the engine executable, a code writing the HUD-visible byte every retrace -> must differ
  cond-false the same write behind a condition that never holds  -> must equal base-off
  cond-true  the same write behind a condition that always holds -> must equal `on`

The code is `--code-lines` text built here from two numbers (the PAL address of the byte and the value), so the
repository carries no third-party code file. Usage:
  python -I tools/mods/codemods/engine_proof.py --base BASE.exe --exe CAND.exe --game GAME_DATA --work WORK
         [--addr 0x8057E8A8] [--value 0] [--tick 810000000] [--tape-dir DIR]
Prints one line per run and a verdict per check; exit code 0 only when every check holds.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

DEFAULT_TAPE_DIR = "H:/ShadowTheHedgehogDiskGC_Extract/source/.build/platform/ab-westopolis/host-a"


def run(tag, exe, args, code_lines):
    d = os.path.join(args.work, tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    for f in ("card.raw", "SRAM.pc.raw", "display.cfg", "pad.cfg"):
        shutil.copy(os.path.join(args.tape_dir, f), d)
    cmd = [exe, "--game-data", args.game, "--boot-stage", "stg0100", "--headless", "--no-audio", "--memory-card", "card.raw",
           "--display-cfg", "display.cfg", "--pad-cfg", "pad.cfg", "--state-file", "unused.gekkostate", "--snapshot-key", "none",
           "--no-xinput", "--budget-ms", "60000", "--input-tape-play", os.path.join(args.tape_dir, "input.tape").replace("\\", "/"),
           "--native-output", "1280x720", "--width", "1280", "--height", "720",
           "--capture-at-tick", "%d:cap.png" % args.tick, "--stop-after-capture"]
    if code_lines is not None:
        with open(os.path.join(d, "code.txt"), "w") as f:
            f.write(code_lines)
        cmd += ["--code-lines", "code.txt", "--code-report", "code.json"]
    env = dict(os.environ, SHADOW_R2_MSAA="1")
    p = subprocess.run(cmd, cwd=d, env=env, capture_output=True, text=True, timeout=900)
    png = os.path.join(d, "cap.png")
    h = hashlib.sha256(open(png, "rb").read()).hexdigest()[:12] if os.path.exists(png) else None
    rep = None
    if code_lines is not None and os.path.exists(os.path.join(d, "code.json")):
        rep = json.load(open(os.path.join(d, "code.json")))["engine"]
    print("%-11s exit=%d picture=%s %s" % (tag, p.returncode, h, ("engine=" + json.dumps({k: rep[k] for k in ("frames", "writes", "writes_changed", "conditions_true", "conditions_false")})) if rep else ""))
    return h, rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--addr", default="0x8057E8A8")
    ap.add_argument("--value", default="0")
    ap.add_argument("--tick", type=int, default=810000000)
    ap.add_argument("--tape-dir", default=DEFAULT_TAPE_DIR)
    args = ap.parse_args()
    off = int(args.addr, 16) & 0x01FFFFFF
    val = int(args.value, 0) & 0xFF
    write = "%08X %08X\n" % (off, val)  # 00: 8-bit write at 0x80000000 + off, one byte
    on = write
    cond_false = "20%06X DEADBEEF\n" % (off & 0xFFFFFF) + write + "E2000001 80008000\n"   # 32-bit equal to a value the byte never has
    cond_true = "22%06X DEADBEEF\n" % (off & 0xFFFFFF) + write + "E2000001 80008000\n"    # not equal: holds
    r = {}
    r["base-off"], _ = run("base-off", args.base, args, None)
    r["cand-off"], _ = run("cand-off", args.exe, args, None)
    r["on"], rep_on = run("on", args.exe, args, on)
    r["cond-false"], rep_f = run("cond-false", args.exe, args, cond_false)
    r["cond-true"], rep_t = run("cond-true", args.exe, args, cond_true)
    checks = [
        ("cand-off == base-off (zero cost off)", r["cand-off"] is not None and r["cand-off"] == r["base-off"]),
        ("on != base-off (the written byte changed the picture)", r["on"] is not None and r["on"] != r["base-off"]),
        ("on wrote once per retrace", bool(rep_on) and rep_on["writes"] == rep_on["frames"] > 0),
        ("cond-false == base-off, 0 writes", r["cond-false"] == r["base-off"] and bool(rep_f) and rep_f["writes"] == 0),
        ("cond-true == on", r["cond-true"] == r["on"] and bool(rep_t) and rep_t["writes"] > 0),
    ]
    ok = True
    for name, good in checks:
        print("%-4s %s" % ("PASS" if good else "FAIL", name))
        ok = ok and good
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
