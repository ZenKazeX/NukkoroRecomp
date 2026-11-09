"""ctest entry for F1c host-stack integrity (test-only): f1c_host_stack_integrity, _notail and f1c_host_stack_game.

For every loop shape x MSR[EE] in {0, 1} x sampler in {tick1, none}, runs the stack driver (a fresh process per
job, so a host crash is a reported result, not a lost run), entered through Cpu::run() (native time: no
instruction budget exists). The fixture driver (f1c_stack_driver, --generated given) runs, on one thread:
three calibration runs that stop at once, 1,000 warm-up iterations, N = iterations / 4 (250,000) and
4N = iterations (1,000,000), every run with the host stack PAINTED below its frame; the production-object
driver (f1c_game_stack_driver, no --generated) runs the warm-up and the main run, sampled by its probe callee
on every iteration.

Pass criterion, per job - the host stack depth does not grow with guest iterations and stays bounded:
  painted  (fixture driver; required there) the painted depths of the N and 4N runs are equal within
           PAINT_SLACK bytes, the 4N depth exceeds the job's calibrated stop-at-once depth by at most
           HOST_STACK_BOUND (difftest_driver.cpp's host-stack bound), and no run overwrote the whole paint
           window. Painting needs no host call inside the loop, so it measures every shape, including the
           tiny ones the generator compiles into one native body with no host call (the probe sampler finds
           no sample point there, below).
  tick1    the probe-sampled host stack watermark after 1,000,000 iterations equals the watermark after
           1,000 iterations, exactly. The probe is a declining HLE hook at the shape's translated record
           heads, the shared leaf and its served callee (f1c_stack_driver.cpp; `tick1` names it for
           history: the retire_block hardware tick it used to name is deleted with the instruction clock).
           A run whose loop reached no probe reports "no-sample-point"; that is accepted only for the
           shapes gen_f1c.py builds without a host call per iteration (the tiny `_t` style without a served
           callee: f1c_fixture.json style "t", hook 0) and only where the painted measurement applies.
  all      the thread's committed stack (NT_TIB StackLimit, measured after painting, so it sees growth
           beyond the paint window) grows by at most 8 KiB over the main run, the guard page is still below
           the committed stack, every run ended where the production dispatcher stops (the untranslated
           sentinel) with r3 == its iteration count and r1 restored.

For an unbounded job the report gives bytes per iteration (probe checkpoints 1k..1M, or the painted growth
from N to 4N), the iteration at which the production game thread's 16 MiB reserve would overflow, and the
per-iteration cycle of host CALL sites that keep their frames (backtrace at iteration 1,000 mapped through the
linker map), cross-checked against census_f1c.py's static list of CALL + epilogue + RET transfer sites.

Exit 0 only if every job is bounded and the guest-state cross-check (--ref-driver) finds no difference.
"""
from __future__ import annotations

import argparse
import bisect
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import census_f1c  # noqa: E402

PRODUCTION_RESERVE = 16 * 1024 * 1024
COMMIT_SLACK = 8 * 1024
# Painted depth: N vs 4N may differ by less than one host frame in total (a per-iteration leak of even one
# 16-byte frame would add 12 MB over the 750,000 iterations between them), and the 4N run may reach at most this
# far below the job's stop-at-once depth.
PAINT_SLACK = 64
HOST_STACK_BOUND = 16 * 1024
SAMPLED = ("tick1", "hook")
MAP_SYM = re.compile(r"^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+f\s+(?:i\s+)?(\S+)\s*$")
PREFERRED_BASE = 0x140000000


def run_job(driver: Path, work: Path, shape: str, ee: int, sampler: str, iterations: int, warm: int,
            reserve_mib: int, timeout: int) -> dict:
    out = work / f"{shape}.ee{ee}.{sampler}.jsonl"
    if out.exists():
        out.unlink()
    cmd = [str(driver), "--shape", shape, "--ee", str(ee), "--sampler", sampler, "--iterations", str(iterations),
           "--warm", str(warm), "--reserve-mib", str(reserve_mib), "--out", str(out)]
    started = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        rc, stderr = proc.returncode, proc.stderr
    except subprocess.TimeoutExpired:
        rc, stderr = "timeout", ""
    record = None
    if out.exists():
        lines = out.read_text(encoding="utf-8").splitlines()
        if lines:
            record = json.loads(lines[-1])
    return {"shape": shape, "ee": ee, "sampler": sampler, "rc": rc, "stderr": stderr[-2000:],
            "wall_s": round(time.monotonic() - started, 2), "record": record, "command": cmd}


def load_map(map_path: Path):
    symbols = []
    for line in map_path.read_text(encoding="latin-1").splitlines():
        m = MAP_SYM.match(line)
        if m:
            symbols.append((int(m.group(2), 16) - PREFERRED_BASE, census_f1c.demangle_plain(m.group(1))))
    symbols.sort()
    return symbols


def symbolize(symbols, rva: int) -> str:
    keys = [s[0] for s in symbols]
    i = bisect.bisect_right(keys, rva) - 1
    if i < 0:
        return f"?+0x{rva:x}"
    return f"{symbols[i][1]}+0x{rva - symbols[i][0]:x}"


def call_before(exe_text, symbols, rva: int) -> dict:
    """The CALL instruction that returns to `rva` (the instruction ending exactly at rva)."""
    import capstone
    data, text_rva = exe_text
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for length in (5, 2, 3, 6, 7, 4):
        start = rva - length - text_rva
        if start < 0:
            continue
        insns = list(md.disasm(data[start:start + length], PREFERRED_BASE + rva - length))
        if len(insns) == 1 and insns[0].mnemonic == "call" and insns[0].size == length:
            op = insns[0].op_str
            target = symbolize(symbols, int(op, 16) - PREFERRED_BASE) if op.startswith("0x") else op
            return {"call_site": symbolize(symbols, rva - length), "call_rva": f"0x{rva - length:x}",
                    "target": target, "insn": f"call {op}"}
    return {"call_site": symbolize(symbols, rva), "call_rva": None, "target": None, "insn": None}


def backtrace_cycle(record: dict, symbols, exe_text) -> dict:
    frames = [int(x, 16) for x in record["main_run"].get("backtrace_rva", [])]
    if len(frames) < 8:
        return {}
    # The per-iteration cycle: the smallest period p with frames[i] == frames[i + p] over the deepest window.
    best = None
    for p in range(1, len(frames) // 3 + 1):
        window = frames[len(frames) // 3:]
        matches = sum(1 for i in range(len(window) - p) if window[i] == window[i + p])
        if matches >= (len(window) - p) * 0.9:
            best = p
            break
    if best is None:
        return {"period": None, "frames": [symbolize(symbols, f) for f in frames]}
    window = frames[len(frames) // 3: len(frames) // 3 + best]
    return {"period": best, "cycle": [dict(call_before(exe_text, symbols, f), return_rva=f"0x{f:x}") for f in window]}


def sample_free(shape_meta: dict) -> bool:
    """A shape gen_f1c.py builds without a host call in every iteration: the tiny style with no served callee.
    The generator may compile its loop into one native body that makes no host call, so the probe sampler can
    find no sample point there (every `_g`/`_b` shape makes an indirect call per iteration, every served-callee
    shape an HLE call, and must be sampled)."""
    return shape_meta.get("style") == "t" and not shape_meta.get("hook")


def evaluate(job: dict, iterations: int, shape_meta: dict, painting_required: bool) -> dict:
    rec = job["record"]
    verdict = {"shape": job["shape"], "ee": job["ee"], "sampler": job["sampler"], "bounded": False, "reasons": []}
    if rec is None:
        verdict["reasons"].append(f"driver produced no record (rc={job['rc']}): {job['stderr'][-400:]}")
        return verdict
    verdict["control"] = rec["control"]
    painted = "quarter_run" in rec
    if painting_required and not painted:
        verdict["reasons"].append("the driver reports no painted N / 4N runs (the fixture driver must paint every run)")
    if rec["thread_exit"] != "0x00000000":
        verdict["reasons"].append(f"guest thread died with host exception {rec['thread_exit']}")
    runs = [name for name in ("warm_run", "quarter_run", "main_run") if name in rec]
    # "no-sample-point" in every run of a proven sample-free shape: the loop makes no host call, so no probe
    # exists in it and the painted depth measures it instead. Anywhere else - another shape, a run without the
    # painted measurement, or a shape sampled in some runs only - it is a failure.
    unsampled = (job["sampler"] in SAMPLED and painted and sample_free(shape_meta) and
                 all(rec[name]["status"] == "no-sample-point" for name in runs))
    for name in runs:
        status = rec[name]["status"]
        if status != "ok" and not (unsampled and status == "no-sample-point"):
            verdict["reasons"].append(f"{name}: {status} ({rec[name]['stop_detail'][:120]})")
    if not rec["guard_after"]:
        verdict["reasons"].append("stack guard page missing below the committed stack after the run")
    warm, main = rec["warm_run"], rec["main_run"]
    commit_growth = main["commit_after"] - main["commit_before"]
    verdict["commit_growth"] = commit_growth
    if commit_growth > COMMIT_SLACK:
        verdict["reasons"].append(f"committed host stack grew {commit_growth} bytes over {iterations} iterations")
    bpi = None
    if job["sampler"] in SAMPLED and unsampled:
        verdict["watermark"] = "no sample point: the loop makes no host call (measured by painting)"
    elif job["sampler"] in SAMPLED:
        verdict["watermark_warm"] = warm["max_depth"]
        verdict["watermark_main"] = main["max_depth"]
        growth = main["max_depth"] - warm["max_depth"]
        verdict["watermark_growth"] = growth
        if growth != 0:
            verdict["reasons"].append(f"host stack watermark {warm['max_depth']} B after {rec['warm']} iterations, "
                                      f"{main['max_depth']} B after {iterations}")
        cps = {c[0]: c[1] for c in main["checkpoints"]}
        if 1000 in cps and iterations in cps:
            bpi = (cps[iterations] - cps[1000]) / (iterations - 1000)
        else:
            bpi = growth / max(1, iterations - rec["warm"])
        verdict["checkpoints"] = main["checkpoints"]
    if painted:
        quarter = rec["quarter_run"]
        baseline = rec.get("painted_baseline")
        verdict["painted_baseline"] = baseline
        verdict["painted_quarter"] = quarter["painted_depth"]
        verdict["painted_main"] = main["painted_depth"]
        saturated = [name for name in runs if rec[name].get("painted_saturated")]
        if saturated:
            verdict["reasons"].append(f"the host stack overwrote the whole {rec['paint_window']}-byte paint window in "
                                      f"{saturated}")
        painted_growth = main["painted_depth"] - quarter["painted_depth"]
        verdict["painted_growth"] = painted_growth
        quarter_iterations = iterations // 4
        if abs(painted_growth) > PAINT_SLACK:
            verdict["reasons"].append(f"painted host stack {quarter['painted_depth']} B after {quarter_iterations} "
                                      f"iterations, {main['painted_depth']} B after {iterations} (slack {PAINT_SLACK})")
        if baseline is None:
            verdict["reasons"].append(f"no calibration run stopped as expected: {rec.get('calibration_status')}")
        else:
            own = main["painted_depth"] - baseline
            verdict["painted_above_baseline"] = own
            if own > HOST_STACK_BOUND:
                verdict["reasons"].append(f"painted host stack {own} B below the stop-at-once depth, bound "
                                          f"{HOST_STACK_BOUND}")
        if bpi is None:
            bpi = painted_growth / max(1, iterations - quarter_iterations)
    if bpi is None:
        bpi = commit_growth / max(1, iterations)
    verdict["bytes_per_iteration"] = round(bpi, 3)
    if bpi > 0:
        verdict["production_overflow_iteration"] = int(PRODUCTION_RESERVE / bpi)
    verdict["dispatches"] = main["dispatches"]
    verdict["bounded"] = not verdict["reasons"]
    return verdict


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--generated", type=Path, default=None,
                        help="gen_f1c.py output (f1c_fixture.json); omit for a driver that lists its own shapes "
                             "(f1c_game_stack_driver --list), which also skips the static census and the painted "
                             "criterion (that driver samples every iteration through its probe callee)")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--warm", type=int, default=1000)
    parser.add_argument("--reserve-mib", type=int, default=1024)
    parser.add_argument("--shapes", default="all")
    parser.add_argument("--ee", default="0,1")
    parser.add_argument("--samplers", default="tick1,none")
    parser.add_argument("--jobs", type=int, default=max(1, min(4, (os.cpu_count() or 2) // 2)))
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--ref-driver", type=Path, default=None,
                        help="f1c_stack_driver_ref (shadow_runtime_reference): cross-check every shape's final "
                             "architected guest state after --state-iterations against the reference interpreter")
    parser.add_argument("--state-iterations", type=int, default=1000)
    args = parser.parse_args()
    args.driver = args.driver.resolve()

    if args.generated is not None:
        fixture = json.loads((args.generated / "f1c_fixture.json").read_text(encoding="utf-8"))
    else:
        listed = subprocess.run([str(args.driver), "--list"], capture_output=True, text=True, check=True).stdout.split()
        fixture = {"shapes": [{"name": n, "kind": "production translated cycle (shadow_recomp)"} for n in listed],
                   "generator_command": ["shadow_recomp (production build)"]}
    painting_required = args.generated is not None
    shapes = [s["name"] for s in fixture["shapes"]]
    if args.shapes != "all":
        wanted = set(args.shapes.split(","))
        shapes = [s for s in shapes if s in wanted]
    args.work.mkdir(parents=True, exist_ok=True)
    census = (census_f1c.census(args.driver, args.map, fixture) if args.generated is not None
              else {"totals": {}, "shapes": {}, "skipped": "production objects: see the verify-r2 census"})
    (args.work / "census.json").write_text(json.dumps(census, indent=1), encoding="utf-8")

    jobs = [(s, int(e), smp) for s in shapes for e in args.ee.split(",") for smp in args.samplers.split(",")]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_job, args.driver, args.work, s, e, smp, args.iterations, args.warm,
                               args.reserve_mib, args.timeout) for s, e, smp in jobs]
        for f in futures:
            results.append(f.result())

    symbols = load_map(args.map)
    import pefile
    pe = pefile.PE(str(args.driver), fast_load=True)
    text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
    exe_text = (text.get_data(), text.VirtualAddress)

    meta = {s["name"]: s for s in fixture["shapes"]}
    verdicts = []
    for job in results:
        v = evaluate(job, args.iterations, meta.get(job["shape"], {}), painting_required)
        if not v["bounded"] and job["record"] is not None and job["sampler"] in SAMPLED:
            v["host_call_cycle"] = backtrace_cycle(job["record"], symbols, exe_text)
        v["census_call_ret_sites"] = census["shapes"].get(job["shape"], {}).get("call_ret", [])
        verdicts.append(v)

    crosscheck = {"ran": False, "mismatches": [], "compared": 0}
    if args.ref_driver is not None:
        crosscheck["ran"] = True
        states = {}
        for engine, exe in (("native", args.driver), ("interp", args.ref_driver.resolve())):
            for ee in args.ee.split(","):
                out = args.work / f"states.{engine}.ee{ee}.jsonl"
                if out.exists():
                    out.unlink()
                r = subprocess.run([str(exe), "--state", str(args.state_iterations), "--shape", "all", "--ee", ee,
                                    "--engine", engine, "--out", str(out)], capture_output=True, text=True,
                                   timeout=args.timeout)
                if r.returncode != 0 or not out.exists():
                    crosscheck["mismatches"].append(
                        f"{engine} ee{ee}: state driver failed rc={r.returncode} {r.stderr[-300:]}")
                    continue
                for line in out.read_text(encoding="utf-8").splitlines():
                    rec = json.loads(line)
                    states[(rec["shape"], int(rec["ee"]), engine)] = rec
        for s_name in shapes:
            for ee in [int(x) for x in args.ee.split(",")]:
                n, i = states.get((s_name, ee, "native")), states.get((s_name, ee, "interp"))
                if n is None or i is None:
                    continue
                crosscheck["compared"] += 1
                if n["status"] != "ok" or i["status"] != "ok":
                    crosscheck["mismatches"].append(
                        f"{s_name} ee{ee}: status native={n['status']} interp={i['status']}")
                    continue
                # "state" carries no instruction count (native time); the interpreter's own count is outside it.
                diff = sorted(k for k in set(n["state"]) | set(i["state"]) if n["state"].get(k) != i["state"].get(k))
                if diff:
                    crosscheck["mismatches"].append(
                        f"{s_name} ee{ee}: guest state differs in {diff}: " +
                        "; ".join(f"{k} native={n['state'].get(k)} interp={i['state'].get(k)}" for k in diff[:4]))
    failing = [v for v in verdicts if not v["bounded"]]
    report = {"schema": "gekko-f1c-stack-report/2", "iterations": args.iterations, "warm": args.warm,
              "painting_required": painting_required, "paint_slack": PAINT_SLACK,
              "host_stack_bound": HOST_STACK_BOUND, "driver": str(args.driver),
              "fixture_generator_command": fixture["generator_command"],
              "jobs": len(verdicts), "bounded": len(verdicts) - len(failing), "unbounded": len(failing),
              "census_totals": census["totals"], "guest_state_crosscheck": crosscheck, "verdicts": verdicts,
              "raw": results}
    (args.work / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")

    print(f"F1c host-stack integrity: {len(verdicts)} jobs ({len(shapes)} shapes x EE {args.ee} x samplers "
          f"{args.samplers}), {args.iterations} iterations each after {args.warm} warm-up"
          + (f"; painted N = {args.iterations // 4} vs 4N = {args.iterations}" if painting_required else ""))
    print(f"{'shape':16} {'ee':>2} {'sampler':7} {'verdict':9} {'B/iter':>8} {'watermark 1k -> 1M':>24} "
          f"{'painted N -> 4N (base)':>24} {'commit growth':>13}  kind")
    for v in verdicts:
        if v["sampler"] in SAMPLED:
            wm = (f"{v.get('watermark_warm', '-')} -> {v.get('watermark_main', '-')}" if "watermark_warm" in v
                  else "no sample point")
        else:
            wm = "-"
        pd = (f"{v['painted_quarter']} -> {v['painted_main']} ({v.get('painted_baseline')})"
              if "painted_quarter" in v else "-")
        print(f"{v['shape']:16} {v['ee']:>2} {v['sampler']:7} {'bounded' if v['bounded'] else 'UNBOUNDED':9} "
              f"{v.get('bytes_per_iteration', 0):>8} {wm:>24} {pd:>24} {v.get('commit_growth', 0):>13}  "
              f"{meta.get(v['shape'], {}).get('kind', '')}")
    for v in failing:
        print(f"\n{v['shape']} (EE={v['ee']}, {v['sampler']}): " + "; ".join(v["reasons"]))
        if "production_overflow_iteration" in v:
            print(f"  16 MiB production reserve exhausted after ~{v['production_overflow_iteration']} iterations")
        cyc = v.get("host_call_cycle", {})
        for frame in cyc.get("cycle", []) or []:
            print(f"  frame kept per iteration: {frame['insn']} at {frame['call_site']} -> {frame['target']}")
    if crosscheck["ran"]:
        print(f"\nguest-state cross-check (native production lane vs reference interpreter, {args.state_iterations} "
              f"iterations): {crosscheck['compared']} shape/EE pairs, {len(crosscheck['mismatches'])} mismatches")
        for m in crosscheck["mismatches"][:40]:
            print("  GUEST STATE MISMATCH: " + m)
    print(f"\n{len(failing)} of {len(verdicts)} jobs UNBOUNDED" if failing else "\nall jobs bounded")
    print(f"report: {args.work / 'report.json'}")
    return 1 if failing or crosscheck["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main())
