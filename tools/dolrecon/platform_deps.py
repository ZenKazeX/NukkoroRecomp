"""The GameCube dependency manifest of PAL main.dol: every platform service entry the program reaches, every
instruction that touches GameCube hardware or depends on PowerPC / Gekko semantics, and what reaches them.

    python -B -m dolrecon.platform_deps <dtk project dir> [--owners <native-owners.txt> ...]
      writes <project>/platform_manifest.json

Evidence kinds (every record says which):
  name          the function's name (pal-v30: matched source, SDK self-names, RenderWare alignment - names.py rules)
  link-order    an unnamed SDK function between two named functions of the same SDK library at most SPAN bytes
                apart (a library's objects are linked contiguously); otherwise "sdk:unidentified"
  address       a memory access whose effective address the must-analysis of dolrecon.vcalls proves constant
                (constant / r2 / r13 bases): MMIO 0xCC000000..0xCC00FFFF, low memory 0x80000000..0x800030FF, the
                uncached mirror 0xC0000000.., the locked cache 0xE0000000..
  instruction   the opcode itself: paired-single arithmetic (psq_* through the stack frame = register save, kept
                apart), GQR / HID / WPAR / DMA / L2CR / DEC / timebase special registers, FPSCR access, the Gekko's
                estimate instructions (fres, frsqrte), fused multiply-add, byte-reversed loads / stores, cache ops
Reach: the call graph of dolrecon.callgraph (call, tail, addr, data, vtable, virtual) from every game function to
every service entry - the services each game function depends on directly and transitively.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import csv
import json
import os
import re
import sys

from . import dol as dolmod
from . import ppc
from . import spine
from . import strrefs
from . import vcalls

SPAN = 0x2000
CALL_KINDS = ("call", "tail", "virtual")
# SDK library by name; service by library (and OS function family)
LIBRARY = [
    # sdk-identify: exact-name rules for newly identified SDK statics whose name carries no library prefix
    # (or whose prefix the regexes below misread, e.g. SPEC2_ matches the audio SP rule)
    (r"^DMAErrorHandler$", "os.interrupts"),  # 0x803726D8
    (r"^GetFontCode$", "os.font"),  # 0x80374E68
    (r"^Decode$", "os.font"),  # 0x80374FDC
    (r"^ReadFont$", "os.font"),  # 0x803751B4
    (r"^ParseStringS$", "os.font"),  # 0x80375618
    (r"^OSGetFontTexel$", "os.font"),  # 0x80375754
    (r"^ExpandFontSheet$", "os.font"),  # 0x803759F4
    (r"^cb$", "files.dvd"),  # 0x8037E03C
    (r"^__fstLoad$", "os.core"),  # 0x8037E114
    (r"^getTiming$", "graphics.vi"),  # 0x8037E578
    (r"^UpdateOrigin$", "graphics.vi"),  # 0x8038041C
    (r"^SPEC2_MakeStatus$", "input.pad"),  # 0x803817D4
    (r"^OnReset$", "input.pad"),  # 0x80381C44
    (r"^SamplingHandler$", "input.pad"),  # 0x80381D00
    (r"^__ReverbHiCreate$", "audio.ax"),  # 0x8038779C
    (r"^__ReverbSTDCreate$", "audio.ax"),  # 0x803886A4
    (r"^__ReverbSTDCallback$", "audio.ax"),  # 0x80388B20
    (r"^__ReverbSTDFree$", "audio.ax"),  # 0x80388ED4
    (r"^__ChorusInit$", "audio.ax"),  # 0x803896FC
    (r"^UnlockedCallback$", "saves.card"),  # 0x8038D788
    (r"^FormatCallback$", "saves.card"),  # 0x80391780
    (r"^CreateCallbackFat$", "saves.card"),  # 0x80392550
    (r"^ReadCallback$", "saves.card"),  # 0x80392A58
    (r"^DeleteCallback$", "saves.card"),  # 0x80393000
    (r"^UpdateIconOffsets$", "saves.card"),  # 0x803931B4
    (r"^powf$", "host.c-library"),  # 0x803B2F50
    (r"^SetExiInterruptMask$", "bus.exi"),  # 0x803B3AB4
    (r"^InitializeUART$", "os.debug"),  # 0x803B57D8
    (r"^WriteUARTN$", "os.debug"),  # 0x803B5848
    # reviewed identities (test_data/platform/sdk_anchor_names.csv): names the regexes below file under os.other / debug.trk
    (r"^__DBExceptionDestination(Aux)?$", "os.debug"),  # 0x80370C18
    (r"^(UnsetRun|SetEffectivePriority|__OSUnlockAllMutex|__OSGetEffectivePriority|DefaultSwitchThreadCallback)$", "os.threads"),  # thread scheduler statics
    (r"^OSGetStackPointer$", "os.core"),  # 0x80372D40
    (r"^(__)?OS(Thread|Mutex|Cond|Message|Sleep|Wakeup|Yield|Suspend|Resume|Join|Detach|Cancel|Exit|Create|SetThread|GetThread|InitThreadQueue|ClearStack|CheckActiveThreads|DisableScheduler|EnableScheduler|Reschedule|SetIdle|SelectThread|GetCurrentThread|IsThread)", "os.threads"),
    (r"^(__)?OS(Alarm|SetAlarm|CancelAlarm|CreateAlarm|SetPeriodicAlarm|InsertAlarm|DecrementerHandler)|^Decrem", "os.alarms"),
    (r"^(__)?OS(Get|Set)?(Time|Tick|SystemTime|TicksToCalendarTime)", "os.time"),
    (r"^(__)?OS(Interrupt|Disable|Enable|Restore|DispatchInterrupt|SetInterruptHandler|GetInterruptHandler|MaskInterrupts|UnmaskInterrupts|SetInterruptMask|Exception|SetErrorHandler|Error)", "os.interrupts"),
    (r"^(__)?OS(Alloc|Free|InitAlloc|CreateHeap|DestroyHeap|SetCurrentHeap|Arena|GetArena|SetArena|AllocFromArena|PhysicalToCached|CachedToPhysical|Protect|InitMemory|GetPhysicalMem|GetConsoleSimulatedMem)", "os.memory"),
    (r"^(__)?OS(Reset|Reboot|GetResetCode|RegisterResetFunction|ShutdownSystem|GetResetSwitch|ExecuteApploader|GetSaveRegion)", "os.reset"),
    (r"^(__)?OS(Sram|LockSram|UnlockSram|GetSoundMode|SetSoundMode|GetProgressiveMode|SetProgressiveMode|GetVideoMode|SetVideoMode|GetLanguage|GetWirelessID|SetWirelessID|GetEuRgb60Mode|ReadROM|SyncSram|InitSram)", "os.sram"),
    (r"^(__)?OS(Font|LoadFont|GetFontTexture|GetFontWidth|GetFontEncode|InitFont)", "os.font"),
    (r"^(__)?OS(Report|Panic|Fatal|VReport|Halt)", "os.debug"),
    (r"^(__)?OS(Init|GetConsoleType|BootInfo|Get(Inner|Outer)?Version|SetFPException|GetLogicalMem|ClearContext|LoadContext|SaveContext|SwitchFPUContext|FillFPUContext|DumpContext|SetCurrentContext|GetCurrentContext|InitContext|Context)", "os.core"),
    (r"^(__)?OS", "os.other"),
    (r"^(__)?(PPC)", "cpu.config"), (r"^(__)?(DC|IC|LC|L2)[A-Z]", "cpu.cache"),
    (r"^(__)?(DVD|dvd)", "files.dvd"), (r"^(__)?(CARD)", "saves.card"),
    (r"^(__)?(PAD|Pad)", "input.pad"), (r"^(__)?SI[A-Z_]", "input.si"),
    (r"^(__)?(GX|GD)[A-Z_]", "graphics.gx"), (r"^(__)?VI[A-Z_]", "graphics.vi"),
    (r"^(__)?(AX|MIX|SYN|SP|SEQ)[A-Z_]", "audio.ax"), (r"^(__)?AI[A-Z_]", "audio.ai"),
    (r"^(__)?(ARQ)[A-Z_]", "memory.aram-queue"), (r"^(__)?AR[A-Z_]", "memory.aram"), (r"^(__)?DSP[A-Z_]", "audio.dsp"),
    (r"^(__)?EXI[A-Z_]", "bus.exi"), (r"^(__)?(DB|TRK|gdev|ddh|udp|nub|InitMetroTRK|TRK_)", "debug.trk"),
]
LIBRARY = [(re.compile(p), s) for p, s in LIBRARY]
MMIO = [(0xCC000000, "graphics.cp"), (0xCC001000, "graphics.pe"), (0xCC002000, "graphics.vi"), (0xCC003000, "cpu.pi"),
        (0xCC004000, "memory.mi"), (0xCC005000, "audio.dsp"), (0xCC006000, "files.di"), (0xCC006400, "input.si"),
        (0xCC006800, "bus.exi"), (0xCC006C00, "audio.ai"), (0xCC008000, "graphics.gx-fifo"), (0xCC010000, None)]
SPR = {912 + i: f"GQR{i}" for i in range(8)}
SPR.update({1008: "HID0", 1009: "HID1", 920: "HID2", 921: "WPAR", 922: "DMA_U", 923: "DMA_L", 1017: "L2CR",
            22: "DEC", 268: "TBL", 269: "TBU", 284: "TBL(w)", 285: "TBU(w)", 1018: "ICTC", 1019: "THRM1", 1020: "THRM2",
            1021: "THRM3", 952: "MMCR0", 956: "MMCR1", 953: "PMC1", 954: "PMC2", 957: "PMC3", 958: "PMC4", 1010: "IABR",
            1013: "DABR", 26: "SRR0", 27: "SRR1", 272: "SPRG0", 273: "SPRG1", 274: "SPRG2", 275: "SPRG3", 19: "DAR",
            18: "DSISR"})
GAME = ("game", "game-unit", "unresolved")


def library_of(name: str):
    for p, s in LIBRARY:
        if p.search(name):
            return s
    return None


def mmio_service(ea: int):
    for i in range(len(MMIO) - 1):
        if MMIO[i][0] <= ea < MMIO[i + 1][0]:
            return MMIO[i][1]
    return None


def region_of(ea: int):
    if 0xCC000000 <= ea < 0xCC010000:
        return "mmio", mmio_service(ea)
    if 0x80000000 <= ea < 0x80003100:
        return "low-memory", "os.globals"
    if 0xC0000000 <= ea < 0xC1800000:
        return "uncached-mirror", "memory.uncached"
    if 0xE0000000 <= ea < 0xE0004000:
        return "locked-cache", "memory.locked-cache"
    return None


def load_layers(skeleton_dir):
    rows = list(csv.DictReader(open(os.path.join(skeleton_dir, "functions.csv"), encoding="utf-8")))
    return {int(r["address"], 16): r for r in rows}


def owned_entries(paths):
    out = {}
    for p in paths:
        if not os.path.exists(p):
            continue
        group = None
        for l in open(p, encoding="utf-8", errors="replace"):
            m = re.match(r"^# (runtime/src/\S+\.cpp)", l)
            if m:
                group = m.group(1)
            m = re.match(r"^0x([0-9A-Fa-f]{8})\b\s*(.*)$", l.strip())
            if m:
                out[int(m.group(1), 16)] = group or os.path.basename(p)
    return out


def analyse(project_dir, owners_paths):
    image = dolmod.load_file(os.path.join(project_dir, "main.dol"))
    funcs, objects = spine.load_symbols(os.path.join(project_dir, "config", "symbols.txt"))
    layers = load_layers(os.path.join(os.path.dirname(os.path.abspath(project_dir)), "skeleton"))
    name = {a: n for a, n, _, _ in funcs}
    size = {a: s for a, _, s, _ in funcs}
    starts = sorted(size)
    text = next(x for x in image.sections if x.kind == "text")
    r2, r13 = strrefs.sda_bases(image, text.address, text.address + text.size)
    sda = {r: v for r, v in ((2, r2), (13, r13)) if v is not None}
    entries = vcalls.interior_entries(image, funcs)
    owners = owned_entries(owners_paths)

    # 1. service entries: SDK functions by name, then by named neighbours in link order
    service, evidence = {}, {}
    sdk = [a for a in starts if layers.get(a, {}).get("layer") == "sdk"]
    for a in sdk:
        s = library_of(name.get(a, ""))
        if s:
            service[a], evidence[a] = s, "name"
    named = [a for a in sdk if a in service]
    for a in sdk:
        if a in service:
            continue
        i = bisect.bisect_left(named, a)
        if 0 < i < len(named) and service[named[i - 1]].split(".")[0] == service[named[i]].split(".")[0] and \
                named[i] - named[i - 1] <= SPAN:
            s = service[named[i - 1]] if service[named[i - 1]] == service[named[i]] else service[named[i - 1]].split(".")[0] + ".other"
            service[a], evidence[a] = s, "link-order"
        else:
            service[a], evidence[a] = "sdk:unidentified", "none"
    for a in starts:
        L = layers.get(a, {}).get("layer")
        if L == "cri":
            service[a], evidence[a] = "middleware.cri", "layer"
        elif L == "rw":
            gc = 0x80465000 <= a < 0x80469000 or 0x80491000 <= a < 0x8049D000
            service[a], evidence[a] = ("graphics.rw-gamecube-driver" if gc else "engine.renderware"), "layer+range"
        elif L == "msl":
            service[a], evidence[a] = "host.c-library", "layer"
        elif L == "runtime":
            service[a], evidence[a] = "host.compiler-runtime", "layer"

    # 2. instruction-level facts of every function (all layers), effective addresses by the must-analysis
    hw = collections.defaultdict(list)          # function -> [(pc, kind, detail, service)]
    ins_re = {
        "paired-single math": lambda w, op, xo: op == 4 and xo not in (6, 38) or op in (56, 57, 60, 61) and False,
    }
    for a, _, s, sec in funcs:
        if not s or not sec.startswith(".text") or layers.get(a, {}).get("layer") in ("sdk",):
            continue
        fn = vcalls.Function(image, a, s, entries.get(a, ()))
        fn.sda = sda
        facts = hw[a]

        def visit(pc, w, regs, consts):
            op = w >> 26
            _, d, ra, _, imm = ppc.fields(w)
            if op in vcalls.MEMORY_D or op in (56, 57, 60, 61):         # D-form loads / stores, psq_l / psq_st
                base = 0 if ra == 0 else sda.get(ra, consts.get(ra))
                if isinstance(base, int):
                    disp = ppc.simm(imm) if op not in (56, 57, 60, 61) else (((imm & 0xFFF) ^ 0x800) - 0x800)
                    ea = (base + disp) & 0xFFFFFFFF
                    r = region_of(ea)
                    if r:
                        facts.append((pc, r[0], f"0x{ea:08X}", r[1]))
            if op == 4:
                xo5, xo10 = (w >> 1) & 0x1F, (w >> 1) & 0x3FF
                if xo10 in (528, 560, 592, 624):
                    facts.append((pc, "paired-single lane move", f"ps_merge xo {xo10}", "cpu.paired-single"))
                elif xo10 in (32, 0, 64, 72, 136, 264, 40, 1014):
                    facts.append((pc, "paired-single other", f"xo {xo10}", "cpu.paired-single"))
                else:
                    facts.append((pc, "paired-single math", f"xo {xo5}/{xo10}", "cpu.paired-single"))
            elif op in (56, 57, 60, 61):
                if ra == 1:
                    facts.append((pc, "paired-single register save", "psq through r1", "cpu.abi"))
                else:
                    gqr = (w >> 12) & 7
                    facts.append((pc, "paired-single load/store", f"GQR{gqr}", "cpu.paired-single"))
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                if xo in (339, 467) and (((w >> 16) & 31) | (((w >> 11) & 31) << 5)) not in (1, 8, 9):
                    spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                    facts.append((pc, "special register", ("mfspr " if xo == 339 else "mtspr ") + SPR.get(spr, str(spr)),
                                  "cpu.config" if SPR.get(spr, "").startswith(("HID", "L2", "WPAR", "DMA")) else
                                  "cpu.quantization" if SPR.get(spr, "").startswith("GQR") else
                                  "os.time" if SPR.get(spr, "") in ("DEC", "TBL", "TBU", "TBL(w)", "TBU(w)") else "cpu.supervisor"))
                elif xo in (371,):
                    facts.append((pc, "timebase", "mftb", "os.time"))
                elif xo in (86, 54, 470, 246, 278, 1014, 982):
                    facts.append((pc, "cache operation", {86: "dcbf", 54: "dcbst", 470: "dcbi", 246: "dcbtst", 278: "dcbt",
                                                          1014: "dcbz", 982: "icbi"}[xo], "cpu.cache"))
                elif xo in (534, 662, 790, 918):
                    facts.append((pc, "byte-reversed access", {534: "lwbrx", 662: "stwbrx", 790: "lhbrx", 918: "sthbrx"}[xo],
                                  "cpu.byte-order"))
                elif xo in (598, 854):
                    facts.append((pc, "memory barrier", {598: "sync", 854: "eieio"}[xo], "cpu.ordering"))
                elif xo in (146, 83, 210, 242, 595, 659):
                    facts.append((pc, "supervisor", {146: "mtmsr", 83: "mfmsr", 210: "mtsr", 242: "mtsrin", 595: "mfsr",
                                                     659: "mfsrin"}[xo], "cpu.supervisor"))
            elif op == 63:
                xo10, xo5 = (w >> 1) & 0x3FF, (w >> 1) & 0x1F
                if xo10 in (583, 711, 38, 70, 134, 64):
                    facts.append((pc, "FPSCR access", {583: "mffs", 711: "mtfsf", 38: "mtfsb1", 70: "mtfsb0",
                                                        134: "mtfsfi", 64: "mcrfs"}[xo10], "cpu.fp-mode"))
                elif xo5 == 26:
                    facts.append((pc, "estimate instruction", "frsqrte", "cpu.fp-estimate"))
            elif op == 59 and (w >> 1) & 0x1F == 24:
                facts.append((pc, "estimate instruction", "fres", "cpu.fp-estimate"))
            elif op == 4 and False:
                pass
            elif op == 19 and (w >> 1) & 0x3FF == 50:
                facts.append((pc, "supervisor", "rfi", "cpu.supervisor"))
        fn.scan((), visit, dict(sda))

    # 3. calls into service entries (every edge kind of the call graph), and reach from game code
    with open(os.path.join(project_dir, "callgraph.json"), encoding="utf-8") as f:
        cg = json.load(f)
    by_name = {n: a for a, n in name.items()}
    out_edges = collections.defaultdict(set)      # calls: call, tail, virtual (an invocation)
    ref_edges = collections.defaultdict(set)      # references: addr, data, vtable (an address handed on or held)
    for e in cg["edges"]:
        s_, t_ = by_name.get(e["from"]), by_name.get(e["to"])
        if s_ is None or t_ is None:
            continue
        (out_edges if e["kind"] in CALL_KINDS else ref_edges)[s_].add((t_, e["kind"], e["via"]))
    call_sites = collections.defaultdict(list)
    asm = os.path.join(project_dir, "out", "asm")
    line = re.compile(r"^/\* ([0-9A-F]{8}) .*\*/\t(bl|b)\s+(\S+)")
    from . import callgraph as _cg
    # dtk writes a scope:local function's calls as name_ADDR
    suffixed = {f"{n}_{a:08X}": a for n, a in by_name.items()}
    for fname in sorted(os.listdir(asm)):
        if not _cg.TEXT_FILE.search(fname):  # incl. duplicate units (_text_N.s) and __init_cpp_exceptions.s
            continue
        for l in open(os.path.join(asm, fname), encoding="utf-8", errors="replace"):
            m = line.match(l)
            t = by_name.get(m.group(3), suffixed.get(m.group(3))) if m else None
            if t is not None:
                if t in service:
                    call_sites[t].append(int(m.group(1), 16))

    def owner_of(pc):
        i = bisect.bisect_right(starts, pc) - 1
        return starts[i] if i >= 0 and pc < starts[i] + size[starts[i]] else None

    platform = {a for a, s in service.items() if not s.startswith(("host.", "engine."))}
    reach = {}

    def services_reached(a, stack=None):
        """platform services a function reaches through non-platform code (memoised; cycles cut)."""
        if a in reach:
            return reach[a]
        reach[a] = set()
        acc = set()
        for t, _, _ in out_edges.get(a, ()):
            if t in platform:
                acc.add(service[t])
            elif layers.get(t, {}).get("layer") in GAME:
                acc |= services_reached(t)
            elif service.get(t, "").startswith("engine."):
                acc |= {"via-renderware:" + s for s in engine_reach(t)}
        acc |= {x[3] for x in hw.get(a, []) if x[3] and not x[3].startswith("cpu.abi")}
        reach[a] = acc
        return acc

    engine_memo = {}

    def engine_reach(a):
        """platform services a RenderWare function reaches through RenderWare (its own GameCube use)."""
        if a in engine_memo:
            return engine_memo[a]
        engine_memo[a] = set()
        acc = {x[3] for x in hw.get(a, []) if x[3] and not x[3].startswith("cpu.abi")}
        for t, _, _ in out_edges.get(a, ()):
            if t in platform:
                acc.add(service[t])
            elif service.get(t, "").startswith("engine."):
                acc |= engine_reach(t)
        engine_memo[a] = acc
        return acc

    sys.setrecursionlimit(100000)
    game = [a for a in starts if layers.get(a, {}).get("layer") in GAME]
    direct = {}
    for a in game:
        d = {service[t] for t, _, _ in out_edges.get(a, ()) if t in platform}
        d |= {x[3] for x in hw.get(a, []) if x[3] and not x[3].startswith("cpu.abi")}
        direct[a] = d
    for a in game:
        services_reached(a)

    # 4. entries: callers outside their own library, with sites
    entries_out = []
    for a in sorted(service):
        if service[a].startswith(("host.", "engine.")):
            continue
        callers = sorted({owner_of(pc) for pc in call_sites.get(a, [])} - {None})
        foreign = [c for c in callers if layers.get(c, {}).get("layer") != layers.get(a, {}).get("layer")]
        entries_out.append({"address": f"0x{a:08X}", "name": name.get(a), "layer": layers.get(a, {}).get("layer"),
                            "service": service[a], "evidence": evidence[a], "owned_by": owners.get(a),
                            "callers": len(callers), "foreign_callers": len(foreign),
                            "foreign_sites": [f"0x{pc:08X}" for pc in call_sites.get(a, [])
                                              if owner_of(pc) in set(foreign)][:40]})
    hw_out = []
    for a, facts in hw.items():
        for pc, kind, detail, svc in facts:
            hw_out.append({"function": name.get(a), "function_address": f"0x{a:08X}",
                           "layer": layers.get(a, {}).get("layer"), "site": f"0x{pc:08X}", "kind": kind,
                           "detail": detail, "service": svc})
    # references from game code to platform entries (callbacks handed on, tables holding them): listed, not
    # counted as dependencies - the holder may be a library's own table (MSL's __files holds TRK console functions)
    refs_out = []
    for a in game:
        for t, k, via in ref_edges.get(a, ()):
            if t in platform:
                refs_out.append({"from": name[a], "to": name.get(t), "service": service[t], "kind": k, "via": via})
    summary = {
        "services": collections.Counter(service[a] for a in service),
        "hardware_sites_by_kind": collections.Counter(x["kind"] for x in hw_out),
        "hardware_sites_by_layer": collections.Counter(x["layer"] for x in hw_out),
        "game_functions": len(game),
        "game_direct_by_service": collections.Counter(s for a in game for s in direct[a]),
        "game_transitive_by_service": collections.Counter(s for a in game for s in reach[a]),
        "game_functions_with_any_direct": sum(1 for a in game if direct[a]),
        "game_references_to_platform": len(refs_out),
        "game_functions_with_any_transitive": sum(1 for a in game if reach[a]),
        "owned_entries": len(owners), "owned_entries_found": sum(1 for a in owners if a in service),
        "sdk_unidentified": sum(1 for a in service if service[a] == "sdk:unidentified"),
    }
    return {"schema": "dolrecon.platform-manifest/1", "project": project_dir, "summary": summary,
            "entries": entries_out, "hardware_sites": hw_out,
            "game_dependencies": {name[a]: {"direct": sorted(direct[a]), "transitive": sorted(reach[a])}
                                  for a in game if reach[a]},
            "game_references": refs_out}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project")
    ap.add_argument("--owners", nargs="*", default=[])
    args = ap.parse_args(argv)
    out = analyse(args.project, args.owners)
    with open(os.path.join(args.project, "platform_manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
    print(json.dumps(out["summary"], indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
