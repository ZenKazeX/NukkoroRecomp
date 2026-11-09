"""Spec-model lane for the F1b differential harness (imported by compare_f1b.py).

Runs the blind Gekko spec model (test_data/core-audit/spec: int, branch, loadstore, fp, ps, exc) over
one F1b vector and returns its lane record "S": per-retired-instruction trace, final state, access
log, recognition records, bound violations and residual undefined masks.

Machine loop, per instruction boundary b (= instructions retired so far):
  1. asynchronous recognition (the manual's policy: every boundary): the external line is a level
     asserted before entry (irq_ext 0, the only position native time can express - see gen_f1b.py);
     the decrementer condition is a latch only a step itself can set (a DEC write the spec leaves
     undefined). With MSR[EE] = 1 exc.take_pending() takes the highest-priority condition.
  2. stop when pc is the sentinel or one of the vector's stop addresses.
  3. fetch the word at pc and step exactly one owner module. Words two modules model are stepped by
     the primary (exc for sc/rfi/tw/twi/mtmsr/mfmsr, ps for dcbz_l) and re-run on a copy by the
     secondary; any disagreement is recorded as a spec-internal note.

Retirement (project convention, labelled): an instruction retires when it completes; sc and a
trap complete (SRR0 names the next instruction or is left to the handler), every fault-style
exception (illegal/privileged program, FP unavailable, alignment, DSI) does not. The retired count
is compared with the interpreter lanes only (native lanes have no instruction count).

Time base and decrementer (native time, docs/native-time-20261005/DESIGN.md): guest code runs in zero
game time, so TB and DEC keep their initial values in every engine lane. The runner keeps them constant
too - an instruction that writes DEC or the time base sets them, nothing else moves them - instead of
the retired project clock (12 retired instructions per TB tick, the deleted kCoreCyclesPerTimeBaseTick)
that modelled the instruction clock.

Undefined behaviour: a module that leaves bits undefined applies its defined part; the undefined
bits are then SUBSTITUTED with the engine's own value from the substitution-source lane (I for the
project policy, IR for vectors whose asynchronous recognition policy differs), because every value
is admissible for an undefined bit. Only the bits the spec names are replaced. The source snapshot
is the one taken after the same number of retired instructions; for a step that does not retire
(a fault or an asynchronous entry) the substitution is applied after the next retired instruction,
which in every F1b exception handler is the preamble `addi r23,r23,1` (it touches none of the
substituted locations), or from the source's final state when the lane stops there. A location that
cannot be substituted stays a residual mask; control flow left undefined ('pc'/'exception'/'all')
makes the rest of the vector engine-only. Bounded results (fres/frsqrte estimates) are checked on
the substituted state; a violation is a spec-visible divergence.
"""
from __future__ import annotations

import copy
import importlib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import spec_runner as f1a  # noqa: E402  (variant, bat_translate, window_changes)

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
SENTINEL = 0x80009000
EE = 0x00008000
DR = 0x00000010
IR = 0x00000020
HANDLER_PREAMBLE = 0x3AF70001        # addi r23,r23,1
RESET_SPR = {536: 0x80001FFF, 537: 0x00000002, 538: 0xC0001FFF, 539: 0x0000002A,
             528: 0x80001FFF, 529: 0x00000002}
NON_RETIRING = {"program", "fp_unavailable", "alignment", "dsi", "isi", "machine_check", "checkstop"}
VECTOR_OF = {"external": 0x500, "decrementer": 0x900, "program": 0x700, "trap": 0x700,
             "fp_unavailable": 0x800, "alignment": 0x600, "syscall": 0xC00, "dsi": 0x300,
             "isi": 0x400, "trace": 0xD00, "system_reset": 0x100, "machine_check": 0x200}

_MODS = {}


def load(spec_dir: Path):
    if _MODS:
        return _MODS
    sys.path.insert(0, str(spec_dir))
    import gekko_state
    _MODS["state"] = gekko_state
    for name in ("int", "branch", "loadstore", "fp", "ps", "exc"):
        _MODS[name] = importlib.import_module(name)
    return _MODS


def owners(word: int):
    m = _MODS
    primary = word >> 26
    xo = (word >> 1) & 0x3FF
    exc_words = primary in (17, 3) or (primary == 19 and xo == 50) or (primary == 31 and xo in (4, 83, 146))
    if exc_words:
        return [m["exc"], m["branch"]] if m["exc"].supports(word) else [m["branch"]]
    if primary in (4, 56, 57, 60, 61):
        sec = [m["loadstore"]] if m["loadstore"].supports(word) else []
        return [m["ps"]] + sec
    return [mod for key, mod in m.items() if key != "state" and mod.supports(word)]


# ---------------------------------------------------------------------------------------------
# state <-> comparator vocabulary
# ---------------------------------------------------------------------------------------------
def state_dict(st, retired, clock):
    return {
        "gpr": [f"0x{x & M32:08X}" for x in st.gpr],
        "fpr": [[f"0x{p[0] & M64:016X}", f"0x{p[1] & M64:016X}"] for p in st.fpr],
        "cr": f"0x{st.cr & M32:08X}", "xer": f"0x{st.xer & M32:08X}", "lr": f"0x{st.lr & M32:08X}",
        "ctr": f"0x{st.ctr & M32:08X}", "msr": f"0x{st.msr & M32:08X}", "fpscr": f"0x{st.fpscr & M32:08X}",
        "pc": f"0x{st.pc & M32:08X}", "srr0": f"0x{st.srr0 & M32:08X}", "srr1": f"0x{st.srr1 & M32:08X}",
        "dar": f"0x{st.dar & M32:08X}", "dsisr": f"0x{st.dsisr & M32:08X}", "hid0": f"0x{st.hid0 & M32:08X}",
        "hid2": f"0x{st.hid2 & M32:08X}", "sprg": [f"0x{x & M32:08X}" for x in st.sprg],
        "gqr": [f"0x{x & M32:08X}" for x in st.gqr],
        "dec": f"0x{clock['dec'] & M32:08X}", "tb": f"0x{clock['tb'] & M64:016X}",
        "line_dec": bool(clock["line_dec"]), "line_ext": bool(st.pending.get("external")),
        "retired": retired, "reserve": bool(st.reserve), "reserve_addr": f"0x{st.reserve_addr & M32:08X}",
        "exception": st.exception,
    }


def engine_view(s: dict) -> dict:
    """Driver state -> spec vocabulary (ints). "retired" only where the lane has an instruction count (the
    interpreter lanes); native lanes have none and their records omit the key."""
    u = lambda x: int(x, 16) if isinstance(x, str) else int(x)  # noqa: E731
    out = {f"gpr[{i}]": u(g) for i, g in enumerate(s["gpr"])}
    for i, (a, b) in enumerate(s["fpr"]):
        out[f"fpr[{i}].ps0"] = u(a)
        out[f"fpr[{i}].ps1"] = u(b)
    for k in ("cr", "xer", "lr", "ctr", "msr", "fpscr", "pc", "tb", "reserve_addr"):
        out[k] = u(s[k])
    spr = {int(n): u(v) for n, v in s["spr"]}
    for name, n in (("srr0", 26), ("srr1", 27), ("dar", 19), ("dsisr", 18), ("hid0", 1008), ("hid2", 920),
                    ("dec", 22)):
        out[name] = spr.get(n, 0)
    for i in range(4):
        out[f"sprg[{i}]"] = spr.get(272 + i, 0)
    for i in range(8):
        out[f"gqr[{i}]"] = spr.get(912 + i, 0)
    if "retired" in s:
        out["retired"] = int(s["retired"])
    out["reserve"] = bool(s["reserve"])
    out["line_dec"] = bool(s["line_dec"])
    out["line_ext"] = bool(s["line_ext"])
    return out


def get_field(st, clock, field):
    if field.startswith("gpr["):
        return st.gpr[int(field[4:-1])]
    if field.startswith("fpr["):
        i = int(field[4:field.index("]")])
        return st.fpr[i][int(field[-1])]
    if field.startswith("sprg["):
        return st.sprg[int(field[5:-1])]
    if field.startswith("gqr["):
        return st.gqr[int(field[4:-1])]
    if field == "dec":
        return clock["dec"]
    if field == "tb":
        return clock["tb"]
    if field == "line_dec":
        return int(bool(clock["line_dec"]))
    if field == "line_ext":
        return int(bool(st.pending.get("external")))
    return getattr(st, field)


def set_field(st, clock, field, value):
    if field.startswith("gpr["):
        st.gpr[int(field[4:-1])] = value & M32
    elif field.startswith("fpr["):
        i = int(field[4:field.index("]")])
        st.fpr[i][int(field[-1])] = value & M64
    elif field.startswith("sprg["):
        st.sprg[int(field[5:-1])] = value & M32
    elif field.startswith("gqr["):
        st.gqr[int(field[4:-1])] = value & M32
    elif field == "dec":
        clock["dec"] = value & M32
        st.dec = value & M32
    elif field == "tb":
        clock["tb"] = value & M64
        st.tb = value & M64
    elif field == "line_dec":
        clock["line_dec"] = bool(value)
        st.pending["decrementer"] = bool(value)
    elif field == "line_ext":
        st.pending["external"] = bool(value)
    else:
        setattr(st, field, value & M32)


FULL64 = lambda f: f.startswith("fpr[") or f == "tb"  # noqa: E731
CONTROL = {"pc", "exception"}
REGISTER_NAMES = {"cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar", "dsisr", "hid0", "hid2",
                  "dec", "tb", "reserve_addr", "pc", "exception"}


def full_mask(field):
    if field.startswith("mem@"):
        return 0xFF
    if field in ("line_dec", "line_ext", "exception"):
        return 1
    return M64 if FULL64(field) else M32


def norm_undefined(module, exc):
    """-> ("all", {}) or ("partial", {field: mask}) in the comparator vocabulary. Unknown locations
    raise KeyError (reported as a spec gap)."""
    area = module.AREA
    out = {}

    def put(field, mask):
        if mask is True:
            mask = full_mask(field)
        out[field] = out.get(field, 0) | mask

    und = exc.undefined
    if area == "ps":
        if "all" in und:
            return "all", {}
        for loc in und:
            base = loc.partition(":")[0]
            if loc.startswith("mem:"):
                addr = int(loc[4:].split("+")[0], 16)
                put(f"mem@0x{addr:08X}", module.undefined_mask(exc, f"mem:0x{addr:X}") or
                    module.undefined_mask(exc, loc.split("+")[0]))
            elif base.startswith("fpr["):
                i, k = base[4:].split("][")
                put(f"fpr[{int(i)}].ps{int(k[:-1])}", module.undefined_mask(exc, base))
            elif base in REGISTER_NAMES:
                put(base, module.undefined_mask(exc, base))
            else:
                raise KeyError(loc)
        return "partial", out
    if area == "loadstore":
        got = module.undefined_masks(exc)
        if "all" in got:
            return "all", {}
        for k, m in got.items():
            put(k, m)
        return "partial", out
    if area == "int":
        if "all" in und:
            return "all", {}
        for loc in und:
            if loc.startswith("gpr["):
                put(loc, M32)
            elif loc == "cr[0:2]":
                put("cr", 0xE0000000)
            else:
                raise KeyError(loc)
        return "partial", out
    # branch, exc, fp: dict {location: mask | True}
    items = und.items() if isinstance(und, dict) else ((k, True) for k in und)
    for loc, m in items:
        if loc == "all":
            return "all", {}
        if loc in ("le_effective_point", "power_mode"):
            raise KeyError(loc)
        if loc == "pending.decrementer":
            put("line_dec", True)
        elif loc == "pending.external":
            put("line_ext", True)
        elif loc == "mem":
            raise KeyError("mem (the shape of the step's memory accesses is undefined)")
        elif loc.startswith("mem["):
            ops = exc.defined.get("mem", ())
            kind, addr, width, _value, _mask = ops[int(loc[4:-1])]
            mask = m if m is not True else (1 << (8 * width)) - 1
            for j in range(width):
                byte = (mask >> (8 * (width - 1 - j))) & 0xFF
                if byte:
                    put(f"mem@0x{(addr + j) & M32:08X}", byte)
        elif loc.startswith(("gpr[", "gqr[", "sprg[")) or loc in REGISTER_NAMES:
            put(loc, m)
        elif loc.startswith("fpr["):
            put(loc, m)
        else:
            raise KeyError(loc)
    return "partial", out


def apply_defined(module, exc, st):
    if module.AREA == "ps":
        d = exc.defined_state
        if d is None:
            return False
        for k in ("gpr", "fpr", "cr", "xer", "lr", "ctr", "msr", "fpscr", "srr0", "srr1", "dar", "dsisr",
                  "dec", "tb", "hid0", "hid2", "gqr", "sprg", "pc", "reserve", "reserve_addr", "exception"):
            setattr(st, k, copy.deepcopy(getattr(d, k)))
        st.mem.bytes = dict(d.mem.bytes)
        st.mem.log = list(d.mem.log)
        return True
    exc.apply_defined(st)
    return True


# ---------------------------------------------------------------------------------------------
def build_state(v):
    gs = _MODS["state"]
    st = gs.State()
    s0 = v["state"]
    st.gpr = list(s0["gpr"])
    st.fpr = [list(p) for p in s0["fpr"]]
    st.cr, st.xer, st.lr, st.ctr = s0["cr"], s0["xer"], s0["lr"], s0["ctr"]
    st.msr, st.fpscr = s0["msr"], s0["fpscr"]
    spr = {int(k): val for k, val in s0["spr"].items()}
    st.srr0, st.srr1, st.dar, st.dsisr = spr.get(26, 0), spr.get(27, 0), spr.get(19, 0), spr.get(18, 0)
    st.sprg = [spr.get(272 + i, 0) for i in range(4)]
    st.hid0 = spr.get(1008, 0x0011C464)
    st.hid2 = spr.get(920, 0xE0000000)
    st.gqr = [spr.get(912 + i, 0) for i in range(8)]
    st.iabr = spr.get(1010, 0)
    st.dec = s0["dec"]
    st.tb = s0["tb"]
    st.reserve, st.reserve_addr = s0["reserve"], s0["reserve_addr"]
    st.pc = v["entry"]
    st.pending = {"external": v.get("irq_ext") == 0, "decrementer": False}
    bats = dict(RESET_SPR)
    bats.update({n: val for n, val in spr.items() if 528 <= n <= 543})

    def phys(ea):
        if not (st.msr & DR):
            return ea & M32
        return f1a.bat_translate(ea, bats)[0]

    def wimg(ea):
        if not (st.msr & DR):
            return 0b0011
        return f1a.bat_translate(ea, bats)[1]

    st.data_phys = phys
    st.data_wimg = wimg
    for address, words in v["row_words"].items():
        base = int(address, 16)
        for i, w in enumerate(words):
            st.mem.bytes.update({(base + 4 * i + k) & M32: (w >> (24 - 8 * k)) & 0xFF for k in range(4)})
    for ea, hexbytes in v["mem"]:
        st.mem.load(ea, bytes.fromhex(hexbytes))
    return st


def mem_value(source_mem, initial, ea):
    """Byte at effective address ea in an engine snapshot: its changed-run table (physical) over the
    vector's initial data window."""
    if ea in source_mem:
        return source_mem[ea]
    return initial.get(ea, 0)


def mem_runs(runs):
    out = {}
    for addr, hexbytes in runs:
        base = int(addr, 16) if isinstance(addr, str) else addr
        for i, b in enumerate(bytes.fromhex(hexbytes)):
            out[base + i] = b
    return out


def run_vector(v: dict, source: dict | None) -> dict:
    mods = _MODS
    exc_mod = mods["exc"]
    st = build_state(v)
    initial = dict(st.mem.bytes)
    stops = set(v.get("stops") or []) | {SENTINEL}
    src_trace = (source or {}).get("trace") or []
    irq_ext = v.get("irq_ext")
    if irq_ext not in (None, 0):
        # A line rising at a retired-instruction count: no instruction count exists and a line changes only
        # at a wait point (gen_f1b.py retired these vectors; the drivers refuse to run them).
        raise ValueError(f"irq_ext {irq_ext}: an interrupt at a retired-instruction count has no native-time meaning")

    residual = {}
    subst = []
    notes = []
    internal = []
    bounds = []
    recognitions = []
    trace = []
    log = []
    retired = 0
    # TB / DEC / the decrementer latch as the engines expose them: constant unless a step writes them (the
    # vector's `residue`, the instruction clock's prescaler phase, has no meaning any more).
    clock = {"tb": st.tb, "dec": st.dec, "line_dec": False}
    status = "ok"
    verdict = "defined"
    deferred = {}          # field -> mask, applied after the next retired instruction
    undefined_control_at = None

    def substitute(masks, snap_state, snap_mem, where):
        done = {}
        view = engine_view(snap_state)
        smem = mem_runs(snap_mem) if snap_mem is not None else {}
        for field, mask in masks.items():
            if field in CONTROL:
                continue
            if field.startswith("mem@"):
                ea = int(field[4:], 16)
                cur = st.mem.bytes.get(ea, 0)
                eng = mem_value(smem, initial, ea)
                st.mem.bytes[ea] = (cur & ~mask) | (eng & mask)
            else:
                cur = get_field(st, clock, field)
                eng = int(view.get(field, 0))
                set_field(st, clock, field, (cur & ~mask) | (eng & mask))
            done[field] = mask
        if done:
            subst.append({"at": where, "fields": {k: f"0x{m:X}" for k, m in done.items()}})
        return done

    def take_async(b):
        nonlocal verdict
        if irq_ext is not None and b >= irq_ext:
            st.pending["external"] = True
        st.pending["decrementer"] = bool(clock["line_dec"])
        if not (st.msr & EE) or not (st.pending.get("external") or st.pending.get("decrementer")):
            return
        srr0_before = st.pc
        try:
            name = exc_mod.take_pending(st)
            masks = {}
        except exc_mod.UndefinedBehaviour as u:
            kind, masks = norm_undefined(exc_mod, u)
            if kind == "all":
                raise
            u.apply_defined(st)
            name = st.exception
        clock["line_dec"] = bool(st.pending.get("decrementer"))
        recognitions.append({"boundary": b, "exception": name, "srr0": f"0x{srr0_before & M32:08X}",
                             "vector": f"0x{st.pc & M32:08X}"})
        for f, m in masks.items():
            deferred[f] = deferred.get(f, 0) | m
        if masks and verdict == "defined":
            verdict = "substituted"

    while True:
        try:
            take_async(retired)
        except Exception as e:                  # 'all' undefined at an asynchronous entry
            status = "undefined-all"
            notes.append(f"boundary {retired}: asynchronous entry boundedly undefined: {e}")
            break
        if st.pc in stops:
            break
        if retired >= v["max_steps"] or len(trace) > 20000:
            status = "step-limit"
            break
        word = st.mem._get(st.pc, 4)
        mods_for = owners(word)
        if not mods_for:
            status = "spec-gap"
            notes.append(f"pc 0x{st.pc:08X} word {word:08X}: spec-gap (no module)")
            break
        module = mods_for[0]
        pc_before = st.pc
        st.tb, st.dec = clock["tb"], clock["dec"]
        dec_seen, tb_seen = st.dec, st.tb
        st.pending["decrementer"] = bool(clock["line_dec"])
        pend_seen = st.pending["decrementer"]
        log_mark = len(st.mem.log)
        secondary_pre = None
        if len(mods_for) > 1:
            secondary_pre = copy.deepcopy(st)
        step_masks = {}
        checks = ()
        try:
            module.step(st, word)
        except NotImplementedError as e:
            status = "spec-gap"
            notes.append(f"pc 0x{pc_before:08X} word {word:08X}: not modelled: {e}")
            break
        except Exception as e:                   # UndefinedBehaviour of any module
            if not hasattr(e, "undefined"):
                raise
            if type(e).__name__ == "Unmodelled":
                status = "spec-gap"
                notes.append(f"pc 0x{pc_before:08X} word {word:08X}: unmodelled: {e}")
                break
            try:
                kind, step_masks = norm_undefined(module, e)
            except KeyError as bad:
                status = "spec-gap"
                notes.append(f"pc 0x{pc_before:08X} word {word:08X}: unmappable undefined location {bad}")
                break
            if kind == "all" or (module.AREA == "branch" and e.partial is None):
                status = "undefined-all"
                notes.append(f"pc 0x{pc_before:08X} word {word:08X}: boundedly undefined: {str(e)[:300]}")
                break
            apply_defined(module, e, st)
            checks = getattr(e, "checks", ())
            notes.append(f"pc 0x{pc_before:08X} word {word:08X}: undefined {sorted(step_masks)}: {str(e)[:240]}")
            if CONTROL & set(step_masks):
                undefined_control_at = retired
                status = "undefined-control"
                break
        if secondary_pre is not None:
            internal_check(mods_for[1], secondary_pre, word, st, step_masks, internal, pc_before)
        exc_name = st.exception
        retires = exc_name is None or exc_name not in NON_RETIRING
        # the branch module models DEC writes; the clock follows them
        if st.dec != dec_seen:
            clock["dec"] = st.dec & M32
        if st.tb != tb_seen:
            clock["tb"] = st.tb & M64
        if st.pending.get("decrementer") != pend_seen and "line_dec" not in step_masks:
            clock["line_dec"] = bool(st.pending.get("decrementer"))
        accesses = [[k, f"0x{a:08X}", w, f"{val:0{2 * w}x}"] for k, a, w, val in
                    (entry[:4] for entry in st.mem.log[log_mark:])]
        log.extend(accesses)
        if step_masks and verdict == "defined":
            verdict = "substituted"
        if retires:
            # No clock advance: a retired instruction takes no game time (the retired project clock - a TB
            # tick and a DEC decrement per 12 retired, an edge latch when DEC crossed zero - is deleted).
            retired += 1
            k = len(trace)
            if deferred:
                if word == HANDLER_PREAMBLE and k < len(src_trace):
                    substitute(deferred, src_trace[k]["s"], src_trace[k]["mem"], f"after step {k} (deferred)")
                else:
                    for f, m in deferred.items():
                        residual[f] = residual.get(f, 0) | m
                deferred = {}
            if step_masks:
                if k < len(src_trace):
                    substitute(step_masks, src_trace[k]["s"], src_trace[k]["mem"], f"step {k}")
                else:
                    for f, m in step_masks.items():
                        residual[f] = residual.get(f, 0) | m
        else:
            for f, m in step_masks.items():
                deferred[f] = deferred.get(f, 0) | m
        for c in checks:
            try:
                ok = bool(c(st))
            except Exception as ce:             # a check that cannot be evaluated is recorded
                ok = False
                notes.append(f"check raised {ce!r}")
            if not ok:
                bounds.append({"step": retired - 1, "pc": f"0x{pc_before:08X}", "word": f"0x{word:08X}",
                               "what": "bounded result outside the manual's bound"})
        if retires:
            snap = state_dict(st, retired, clock)
            snap["mem"] = f1a.window_changes(st.mem.bytes, initial)
            snap["word"] = f"0x{word:08X}"
            snap["from_pc"] = f"0x{pc_before:08X}"
            snap["mnemonic"] = module.mnemonic(word)
            snap["variant"] = f1a.variant(snap["mnemonic"], word) if isinstance(snap["mnemonic"], str) else None
            snap["module"] = module.AREA
            snap["access"] = accesses
            trace.append(snap)
        else:
            recognitions.append({"boundary": retired, "exception": exc_name, "srr0": f"0x{st.srr0 & M32:08X}",
                                 "vector": f"0x{st.pc & M32:08X}", "synchronous": True,
                                 "word": f"0x{word:08X}", "from_pc": f"0x{pc_before:08X}"})
        if exc_name is not None and retires:
            recognitions.append({"boundary": retired, "exception": exc_name, "srr0": f"0x{st.srr0 & M32:08X}",
                                 "vector": f"0x{st.pc & M32:08X}", "synchronous": True,
                                 "word": f"0x{word:08X}", "from_pc": f"0x{pc_before:08X}"})
    if deferred:
        fin = (source or {}).get("final")
        if status == "ok" and fin is not None and int(fin["retired"]) == retired and \
                int(fin["pc"], 16) == st.pc:
            substitute(deferred, fin, (source or {}).get("mem"), "final (deferred)")
        else:
            for f, m in deferred.items():
                residual[f] = residual.get(f, 0) | m
    if status in ("undefined-all", "undefined-control") and trace:
        verdict = status
    elif status in ("undefined-all", "undefined-control"):
        verdict = status
    st.tb, st.dec = clock["tb"], clock["dec"]
    final = state_dict(st, retired, clock)
    return {"id": v["id"], "lane": "S", "status": status, "verdict": verdict, "masks": residual,
            "initial_fpscr": v["state"]["fpscr"], "initial_msr": v["state"]["msr"],
            "substitutions": subst, "notes": notes, "internal": internal, "bounds": bounds,
            "recognitions": recognitions, "final": final, "mem": f1a.window_changes(st.mem.bytes, initial),
            "log": log, "trace": trace, "undefined_control_at": undefined_control_at}


def internal_check(secondary, pre, word, primary_post, primary_masks, internal, pc_before):
    """Re-run a doubly-modelled word on the secondary module; record outcome disagreements."""
    try:
        secondary.step(pre, word)
        sec_masks = {}
    except NotImplementedError:
        return
    except Exception as e:
        if not hasattr(e, "undefined"):
            internal.append({"pc": f"0x{pc_before:08X}", "word": f"0x{word:08X}",
                             "secondary": secondary.AREA, "error": repr(e)[:200]})
            return
        try:
            kind, sec_masks = norm_undefined(secondary, e)
        except KeyError:
            return
        if kind == "all":
            return
        apply_defined(secondary, e, pre)
    fields = ["pc", "msr", "srr0", "srr1", "cr", "xer", "lr", "ctr", "dar", "dsisr", "fpscr", "exception"] + \
             [f"gpr[{i}]" for i in range(32)]
    clock = {"dec": 0, "tb": 0, "line_dec": False}
    diffs = {}
    for f in fields:
        a = get_field(primary_post, clock, f) if f != "exception" else primary_post.exception
        b = get_field(pre, clock, f) if f != "exception" else pre.exception
        m = primary_masks.get(f, 0) | sec_masks.get(f, 0)
        if f == "exception":
            if a != b and not m:
                diffs[f] = [a, b]
            continue
        if (a & ~m & M32) != (b & ~m & M32):
            diffs[f] = [f"0x{a & M32:08X}", f"0x{b & M32:08X}"]
    if diffs:
        internal.append({"pc": f"0x{pc_before:08X}", "word": f"0x{word:08X}", "secondary": secondary.AREA,
                         "fields": diffs})
