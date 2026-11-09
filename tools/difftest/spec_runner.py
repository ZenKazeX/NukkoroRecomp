"""Run the blind Gekko spec model (test_data/core-audit/spec) over difftest vectors.

For each vector the machine loop is:
  fetch the word at pc from the spec Memory (unlogged), pick the ONE spec module whose supports()
  accepts it (none -> spec-gap, more than one -> spec-ambiguous), step it, record a per-instruction
  snapshot, stop when pc reaches the sentinel, an exception is taken, or max_steps is exceeded.

Undefined behaviour (the manual leaves results undefined) is not a pass or a failure: the defined
parts are applied (exc.apply_defined) and the undefined locations are converted to field masks
that the comparator applies ONLY to the spec-vs-engine comparison. 'all' (boundedly undefined /
invalid form) makes the vector engine-vs-engine only from that step on.

Time base and decrementer (native time, docs/native-time-20261005/DESIGN.md): guest code runs in zero
game time - the time base and DEC are views of a game clock that only a wait point moves, and these
vectors contain none - so TB and DEC keep their initial values in every engine lane. The spec modules do
not advance them either, and the runner no longer imposes the retired project clock (12 retired
instructions per TB tick, the deleted kCoreCyclesPerTimeBaseTick): an instruction that writes DEC or the
time base (mtspr 22/284/285) sets the value it wrote, nothing else moves them, and the decrementer-
condition latch changes only where a spec step sets it.

Output: one JSON object per vector (JSONL) with the same field vocabulary the driver uses.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

M32 = 0xFFFFFFFF
SENTINEL = 0x80009000


def load_spec(spec_dir: Path):
    sys.path.insert(0, str(spec_dir))
    import gekko_state          # noqa: E402
    import branch as m_branch   # noqa: E402
    import int as m_int         # noqa: E402
    import loadstore as m_ls    # noqa: E402
    return gekko_state, (m_branch, m_int, m_ls)


def bat_translate(ea: int, spr: dict):
    """Data translation through the vector's DBATs (harness function, not runtime code).

    BATU = BEPI (bits 0-14) | BL (19-29) | Vs (30) | Vp (31); BATL = BRPN (0-14) | WIMG (25-28) |
    PP (30-31). Returns (physical, WIMG) or (None, None) when no supervisor-valid DBAT matches."""
    for i in range(4):
        upper = spr.get(536 + 2 * i, 0)
        lower = spr.get(537 + 2 * i, 0)
        if not (upper & 0x2):
            continue
        bl = (upper >> 2) & 0x7FF
        match = 0xFFFE0000 & ~(bl << 17) & M32
        if (ea & match) == (upper & match):
            return ((lower & match) | (ea & ~match & M32)) & M32, (lower >> 3) & 0xF
    return None, None


RESET_SPR = {536: 0x80001FFF, 537: 0x00000002, 538: 0xC0001FFF, 539: 0x0000002A}


def mask_names_int(undefined, masks):
    for loc in undefined:
        if loc == "all":
            return False
        if loc.startswith("gpr["):
            masks[loc] = M32
        elif loc == "cr[0:2]":
            masks["cr"] = masks.get("cr", 0) | 0xE0000000
        else:
            raise KeyError(loc)
    return True


def mask_names_branch(undefined, masks):
    for loc, m in undefined.items():
        if loc == "all":
            return False
        if loc in ("le_effective_point", "power_mode"):
            raise KeyError(loc)          # annotations the harness cannot compare: spec-gap
        if loc == "pending.decrementer":
            masks["line_dec"] = 1
            continue
        masks[loc] = masks.get(loc, 0) | (M32 if m is True else m)
    return True


def mask_names_ls(module, exc, masks):
    got = module.undefined_masks(exc)
    if "all" in got:
        return False
    for loc, m in got.items():
        key = {"pc": "pc", "exception": "exception"}.get(loc, loc)
        masks[key] = masks.get(key, 0) | m
    return True


NON_PROPAGATING = {"line_dec"}
XO_ARITH = {266, 10, 138, 234, 202, 40, 8, 136, 232, 200, 104, 235, 75, 11, 491, 459}
RC_PRIMARY = {20, 21, 23}


def variant(mnemonic, word):
    """Instruction class including the OE ('o') and Rc ('.') variants and the link/absolute bits."""
    if mnemonic is None:
        return None
    primary = word >> 26
    name = mnemonic.rstrip(".")
    if primary == 31:
        xo9 = (word >> 1) & 0x1FF
        if xo9 in XO_ARITH and word & 0x400:
            name += "o"
        if word & 1:
            name += "."
    elif primary in RC_PRIMARY and word & 1:
        name += "."
    elif mnemonic.endswith("."):
        name += "."
    elif primary in (16, 18) or (primary == 19 and ((word >> 1) & 0x3FF) in (16, 528)):
        if word & 1:
            name += "[l]"
        if primary in (16, 18) and word & 2:
            name += "[a]"
    return name


def state_dict(st, retired, spr_extra):
    return {
        "gpr": [f"0x{x & M32:08X}" for x in st.gpr],
        "fpr": [[f"0x{p[0]:016X}", f"0x{p[1]:016X}"] for p in st.fpr],
        "cr": f"0x{st.cr:08X}", "xer": f"0x{st.xer:08X}", "lr": f"0x{st.lr:08X}", "ctr": f"0x{st.ctr:08X}",
        "msr": f"0x{st.msr:08X}", "fpscr": f"0x{st.fpscr:08X}", "pc": f"0x{st.pc:08X}",
        "srr0": f"0x{st.srr0:08X}", "srr1": f"0x{st.srr1:08X}", "dar": f"0x{st.dar:08X}",
        "dsisr": f"0x{st.dsisr:08X}", "hid0": f"0x{st.hid0:08X}", "hid2": f"0x{st.hid2:08X}",
        "sprg": [f"0x{x:08X}" for x in st.sprg], "gqr": [f"0x{x:08X}" for x in st.gqr],
        "dec": f"0x{spr_extra['dec']:08X}", "tb": f"0x{spr_extra['tb']:016X}",
        "line_dec": spr_extra["line_dec"],
        "retired": retired, "reserve": bool(st.reserve), "reserve_addr": f"0x{st.reserve_addr & M32:08X}",
        "exception": st.exception,
    }


def window_changes(mem_bytes: dict, initial: dict) -> list:
    """Changed bytes of the data window as [address, hex] runs (same shape as the driver)."""
    changed = sorted(a for a, b in mem_bytes.items()
                     if 0x80300000 <= a < 0x80308000 and initial.get(a, 0) != b)
    runs, i = [], 0
    while i < len(changed):
        j = i
        while j + 1 < len(changed) and changed[j + 1] == changed[j] + 1:
            j += 1
        start = changed[i]
        data = bytes(mem_bytes[a] for a in range(start, changed[j] + 1))
        runs.append([f"0x{start:08X}", data.hex()])
        i = j + 1
    return runs


def run_vector(gekko_state, modules, v: dict) -> dict:
    st = gekko_state.State()
    s0 = v["state"]
    st.gpr = list(s0["gpr"])
    st.fpr = [list(p) for p in s0["fpr"]]
    st.cr, st.xer, st.lr, st.ctr = s0["cr"], s0["xer"], s0["lr"], s0["ctr"]
    st.msr, st.fpscr = s0["msr"], s0["fpscr"]
    spr = {int(k): val for k, val in s0["spr"].items()}
    st.srr0, st.srr1, st.dar, st.dsisr = spr.get(26, 0), spr.get(27, 0), spr.get(19, 0), spr.get(18, 0)
    st.sprg = [spr.get(272 + i, 0) for i in range(4)]
    st.hid0, st.hid2 = 0x0011C464, 0xE0000000
    st.gqr = [0] * 8
    st.dec = s0["dec"]
    st.tb = s0["tb"]
    st.reserve, st.reserve_addr = s0["reserve"], s0["reserve_addr"]
    st.pc = v["entry"]
    bats = dict(RESET_SPR)
    st.data_wimg = lambda ea: bat_translate(ea, bats)[1]
    st.data_phys = lambda ea: bat_translate(ea, bats)[0]
    for address, words in v["row_words"].items():
        base = int(address, 16)
        for i, w in enumerate(words):
            st.mem.bytes.update({base + 4 * i + k: (w >> (24 - 8 * k)) & 0xFF for k in range(4)})
    for ea, hexbytes in v["mem"]:
        st.mem.load(ea, bytes.fromhex(hexbytes))
    initial = dict(st.mem.bytes)

    masks = {}
    undefined_steps = []
    verdict = "defined"
    notes = []
    trace = []
    log = []
    retired = 0
    # TB / DEC / the decrementer latch as the engines expose them: constant unless an instruction writes them
    # (the vector's `residue`, the instruction clock's prescaler phase, has no meaning any more).
    clock = {"tb": st.tb, "dec": st.dec, "line_dec": False}
    status = "ok"
    while st.pc != SENTINEL:
        if retired >= v["max_steps"]:
            status = "step-limit"
            break
        word = st.mem._get(st.pc, 4)
        owners = [m for m in modules if m.supports(word)]
        if len(owners) != 1:
            status = "spec-gap" if not owners else "spec-ambiguous"
            notes.append(f"pc 0x{st.pc:08X} word {word:08X}: {status} ({[m.AREA for m in owners]})")
            break
        module = owners[0]
        pc_before = st.pc
        # The time base and DEC the instruction sees: their initial values, or what an earlier instruction
        # wrote (guest code takes no game time).
        st.tb, st.dec = clock["tb"], clock["dec"]
        dec_seen, tb_seen = st.dec, st.tb
        log_mark = len(st.mem.log)
        try:
            module.step(st, word)
        except NotImplementedError as exc:       # branch NotModelled
            status = "spec-gap"
            notes.append(f"pc 0x{pc_before:08X} word {word:08X}: not modelled: {exc}")
            break
        except Exception as exc:                 # UndefinedBehaviour of any spec module
            if not hasattr(exc, "apply_defined"):
                raise
            if type(exc).__name__ == "Unmodelled":
                status = "spec-gap"
                notes.append(f"pc 0x{pc_before:08X} word {word:08X}: unmodelled: {exc}")
                break
            undefined_steps.append(retired)
            try:
                if module.AREA == "loadstore":
                    ok = mask_names_ls(module, exc, masks)
                elif isinstance(exc.undefined, dict):
                    ok = mask_names_branch(exc.undefined, masks)
                else:
                    ok = mask_names_int(exc.undefined, masks)
            except KeyError as bad:
                status = "spec-gap"
                notes.append(f"pc 0x{pc_before:08X} word {word:08X}: unmappable undefined location {bad}")
                break
            partial = getattr(exc, "partial", "n/a")
            if not ok or partial is None:
                verdict = "undefined-all"
                notes.append(f"pc 0x{pc_before:08X} word {word:08X}: boundedly undefined: {exc}")
                status = "undefined-all"
                break
            exc.apply_defined(st)
            if verdict == "defined":
                verdict = "partially-undefined"
            notes.append(f"pc 0x{pc_before:08X} word {word:08X}: undefined {sorted(masks)}: {exc}")
            if st.pc == pc_before and getattr(st, "exception", None) is None:
                status = "spec-gap"
                notes.append("defined part did not advance pc")
                break
        retired += 1
        # An instruction that wrote DEC or the time base (mtspr 22/284/285) sets the clock.
        if st.dec != dec_seen:
            clock["dec"] = st.dec & M32
        if st.tb != tb_seen:
            clock["tb"] = st.tb & 0xFFFFFFFFFFFFFFFF
        # No clock advance: the retired project clock (one TB tick and one DEC decrement per 12 retired
        # instructions, a decrementer edge when DEC crossed zero) modelled the deleted instruction clock.
        accesses = []
        for entry in st.mem.log[log_mark:]:
            kind, address, width = entry[0], entry[1], entry[2]
            value = entry[3]
            accesses.append([kind, f"0x{address:08X}", width, f"{value:0{2 * width}x}"])
        log.extend(accesses)
        snap = state_dict(st, retired, clock)
        snap["mem"] = window_changes(st.mem.bytes, initial)
        snap["word"] = f"0x{word:08X}"
        snap["from_pc"] = f"0x{pc_before:08X}"
        snap["mnemonic"] = module.mnemonic(word)
        snap["variant"] = variant(snap["mnemonic"], word)
        snap["access"] = accesses
        trace.append(snap)
        if st.exception is not None:
            status = "exception"
            notes.append(f"pc 0x{pc_before:08X} word {word:08X}: exception {st.exception}")
            break
    if verdict == "partially-undefined":
        # Masks cover the undefined locations of the step that produced them. Any later step
        # other than the terminating bclr could read those locations, so the result would no
        # longer be defined: the vector becomes engine-vs-engine only.
        later = [s for s in trace[undefined_steps[0] + 1:] if s["mnemonic"] != "bclr"]
        # The decrementer-condition latch is read by no instruction these families generate (MSR[EE]
        # stays 0 and nothing enters an exception), so an undefined latch cannot propagate.
        if set(masks) <= NON_PROPAGATING:
            later = []
        if later:
            verdict = "undefined-propagating"
            notes.append(f"undefined result at step {undefined_steps[0]} is followed by {len(later)} "
                         f"more instructions ({later[0]['mnemonic']} first)")
    st.tb, st.dec = clock["tb"], clock["dec"]
    final = state_dict(st, retired, clock)
    return {"id": v["id"], "lane": "S", "status": status, "verdict": verdict, "masks": masks,
            "notes": notes, "final": final, "mem": window_changes(st.mem.bytes, initial), "log": log,
            "trace": trace}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--vectors", type=Path, required=True, help="vectors.jsonl")
    p.add_argument("--spec", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--only", default="")
    a = p.parse_args()
    gekko_state, modules = load_spec(a.spec)
    only = set(x for x in a.only.split(",") if x)
    count = 0
    with a.out.open("w", encoding="utf-8") as out:
        for line in a.vectors.open(encoding="utf-8"):
            v = json.loads(line)
            if only and v["id"] not in only:
                continue
            out.write(json.dumps(run_vector(gekko_state, modules, v)) + "\n")
            count += 1
    print(f"spec_runner: {count} vectors -> {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
