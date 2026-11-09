"""The game's spine: which functions are the game's own logic, which are libraries, which touch the GameCube.

    python -B -m dolrecon.spine <named project> <out dir> [--rw-candidates CSV]        (run from source/tools)

Every function gets ONE layer, with the rule that gave it and the evidence:

  game       Sonic Team code: methods / constructors of the game's RTTI classes, and code linked among them.
  boost      Boost template code (boost:: classes): portable C++, compiled into the game's own units.
  rw         RenderWare Graphics 3.7 (core, plugins, GameCube driver).
  cri        CRI middleware (ADX audio, Sofdec video, SUD/SFH/MW players).
  msl        Metrowerks Standard Library (C library, std:: classes).
  runtime    CodeWarrior runtime (startup, C++ ABI helpers, register save/restore, 64-bit divide).
  sdk        Nintendo Dolphin SDK and MetroTRK: the GameCube operating environment.

Rules, strongest first; a function keeps the first layer it gets:
  A1 name    dtk's exact-byte library names (the dtk-only project's symbols), main -> game.
  A2 rw      STRONG RenderWare candidates from RW37SDK/_mined (static-library anchors).
  A3 class   a function in a class's vtable, naming a class's constructor (names.json N2 / N6 / N7), or building
             or loading the vtable of a class without being named after it (names.json class_uses): its namespace.
  A4 string  a referenced string literal with a library's signature text (CRI error codes, SDK messages).
  S1 between unanchored code between two anchors of the same layer, in link order, with no anchor of another layer
             in between (CodeWarrior links each object file contiguously and libraries as whole archives).
  G1 graph   an unresolved function whose resolved callers and callees all have one layer.
Anything else stays "unresolved". No layer is guessed from an address range alone.

Independently of the layer, every instruction is scanned for GameCube environment use:
  mmio       lis of 0xCC00/0xCC01/0xCD00/0xCD01 (hardware registers, write-gather pipe 0xCC008000)
  uncached   lis of 0xC000-0xC17F (uncached mirror of main memory)
  lockedcache lis of 0xE000 (locked-cache scratch)
  lowmem     lis 0x8000 + a d-form access below 0x3100 (OS globals in low memory)
  spr / msr / timebase / gqr / trap / cache / barrier   supervisor or machine-specific instructions
  psq        paired-single quantized load/store (behaviour depends on GQR configuration)
  ps         paired-single arithmetic (a CPU SIMD feature: portable math, not environment)
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

from . import corrections as corrmod
from . import dol as dolmod
from . import elfobj
from . import strrefs

LAYERS = ["game", "game-unit", "boost", "rw", "cri", "msl", "runtime", "sdk"]
# which layers a layer's code may call (a library never calls the game, the SDK never calls RenderWare)
MAY_CALL = {
    "game": set(LAYERS),
    "game-unit": set(LAYERS),
    "boost": {"boost", "msl", "runtime", "game"},
    "rw": {"rw", "msl", "runtime", "sdk"},
    "cri": {"cri", "msl", "runtime", "sdk"},
    "msl": {"msl", "runtime", "sdk"},
    "runtime": {"runtime", "msl"},
    "sdk": {"sdk", "runtime", "msl"},
}
ENV_FLAGS = {"mmio", "uncached", "lockedcache", "lowmem", "spr", "msr", "timebase", "gqr", "trap", "cache",
             "barrier"}

RUNTIME_NAMES = re.compile(
    r"^(__start|__init_registers|__init_data|__init_hardware|__flush_cache|__check_pad3|__set_debug_bba|"
    r"__get_debug_bba|__init_user|__init_cpp|__fini_cpp|_ExitProcess|__va_arg|__destroy_global_chain|"
    r"__register_global_object|__save_[fg]pr|__restore_[fg]pr|_savegpr_\d+|_restgpr_\d+|_savefpr_\d+|"
    r"_restfpr_\d+|__div2u|__div2i|__mod2u|__mod2i|__shl2i|__shr2u|__shr2i|__cvt_\w+|__init_cpp_exceptions|"
    r"__fini_cpp_exceptions|__register_fragment|__unregister_fragment|__ptmf_\w+|__construct_\w+|"
    r"__destroy_\w+|__dt__\w+|__copy|__ExceptionHandler\w*|__throw\w*|__end_catch|__unexpected|"
    r"__terminate|__dynamic_cast|__get_typeid|__RTTI\w*|__pure_virtual_called|MWTRACE)$")
MSL_NAMES = re.compile(
    r"^(mem(set|cpy|move|chr|cmp)|__memrchr|__fill_mem|__copy_\w+|str\w+|wcs\w+|mbs\w+|f(write|read|seek|tell|"
    r"open|close|flush|puts|gets|printf|wide|putc|getc)|__fwrite|__fread|v?s?n?printf|v?f?printf|sscanf|"
    r"__pformatter|__sformatter|__FileWrite|__StringWrite|float2str|double2hex|long2str|longlong2str|"
    r"parse_format|__stdio_atexit|__close_all|__flush_all|__flush_buffer|__prep_buffer|__(begin|end|kill)_"
    r"critical_regions?|exit|abort|atexit|__exit|__num2dec|__str2\w+|atoi|atof|strtol|strtoul|strtod|rand|"
    r"srand|qsort|bsearch|abs|labs|tolower|toupper|is\w+|sqrtf?|sinf?|cosf?|tanf?|atan2?f?|asinf?|acosf?|"
    r"floorf?|ceilf?|fabsf?|fmodf?|powf?|expf?|logf?|log10f?|ldexpf?|frexpf?|modff?|scalbn|copysign|"
    r"__ieee754_\w+|__kernel_\w+|nan|__fpclassify\w*|__signbit\w*)$")
SDK_PREFIX = re.compile(
    r"^_*(OS|PPC|DC|IC|LC|L2|DB|DVD|VI|PAD|SI|EXI|AI|AR|ARQ|AX|AXFX|MIX|SP|DSP|CARD|GX|GD|THP|DTK|TRK|"
    r"InitMetroTRK|EnableMetroTRK|TRK_|MetroTRK|EXI2|UART|AMC_|Hu_|SetExiInterruptMask|Decrementer|"
    r"ConfigBus|ExternalInterrupt|SelectThread|Alarm|cbFor|state[A-Z]|ErrorCode2Num|__fstLoad|"
    r"InitializeProgramEndTrap|ReserveEXI2Port|UnreserveEXI2Port|Unlocked|GetTypeCallback|SystemCallVector|"
    r"gdev_|ddh_|udp_|usr_|__DVD|__OS|__GX|__VI|__PAD|__SI|__EXI|__AI|__AR|__AX|__CARD|__DSP)")

CRI_TEXT = re.compile(r"(\bE\d{5,}|\bW\d{4}\b|adx|ADX|\(c\)CRI|CRI [A-Z]{2,4}/|mwPly|MWSF|sfx|SFX|Sofdec|"
                      r"RNARES|CRITAGS|<SUDPS_>|STCCODE|SofdecStream)")
SDK_TEXT = re.compile(r"(DVDChangeDisk|OSPanic|DBException|__OS|CARDCheck|GXInit|EXI|Metrowerks CW runtime|"
                      r"SendACK : Calling MessageSend|No buffer available)")
RUNTIME_TEXT = re.compile(r"Metrowerks CW runtime")


def decode_env(image, start: int, size: int) -> collections.Counter:
    """Count GameCube environment instructions in [start, start+size)."""
    flags = collections.Counter()
    lis_regs: dict[int, int] = {}       # register -> instructions since `lis rX, 0x8000`
    for a in range(start, start + size, 4):
        w = image.word(a)
        op = w >> 26
        rd, ra = (w >> 21) & 0x1F, (w >> 16) & 0x1F
        for r in list(lis_regs):
            lis_regs[r] += 1
            if lis_regs[r] > 8:
                del lis_regs[r]
        if op == 15 and ra == 0:                          # lis rD, imm
            imm = w & 0xFFFF
            if imm in (0xCC00, 0xCC01, 0xCD00, 0xCD01):
                flags["mmio"] += 1
            elif 0xC000 <= imm < 0xC180:
                flags["uncached"] += 1
            elif imm == 0xE000:
                flags["lockedcache"] += 1
            if imm == 0x8000:
                lis_regs[rd] = 0
            elif rd in lis_regs:
                del lis_regs[rd]
            continue
        if op in (32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55) and \
                ra in lis_regs:
            d = w & 0xFFFF
            if d < 0x3100:
                flags["lowmem"] += 1
        if op in (14, 24) and ra in lis_regs:             # addi/ori building a full address: not low memory
            del lis_regs[ra]
        if op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in (339, 467):                          # mfspr / mtspr
                spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
                if spr in (1, 8, 9):
                    pass
                elif 912 <= spr <= 919:
                    flags["gqr"] += 1
                else:
                    flags["spr"] += 1
            elif xo == 371:
                flags["timebase"] += 1
            elif xo in (83, 146):
                flags["msr"] += 1
            elif xo in (470, 86, 54, 982):                # dcbi dcbf dcbst icbi
                flags["cache"] += 1
            elif xo in (598, 854):                        # sync eieio
                flags["barrier"] += 1
        elif op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 150:
                flags["barrier"] += 1                     # isync
            elif xo == 50:
                flags["trap"] += 1                        # rfi
        elif op == 17:
            flags["trap"] += 1                            # sc
        elif op in (56, 57, 60, 61):
            flags["psq"] += 1
        elif op == 4:
            xo6 = (w >> 1) & 0x3F
            if (w >> 1) & 0x3FF == 1014:
                flags["cache"] += 1                       # dcbz_l
            elif xo6 in (6, 7, 38, 39):
                flags["psq"] += 1
            else:
                flags["ps"] += 1
    return flags


def load_symbols(path: str):
    funcs, objects = [], {}
    pat = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            a, size = int(m[3], 16), int(m[5] or "0", 16)
            if m[4] == "function":
                funcs.append((a, m[1], size, m[2]))
            else:
                objects[m[1]] = (a, size, m[2], m[4])
    funcs.sort()
    return funcs, objects


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("project", help="named project (pal-v5)")
    ap.add_argument("out")
    ap.add_argument("--dtk-project", help="the dtk-only project whose function names are dtk's own (default: pal)")
    ap.add_argument("--rw-candidates", default=r"H:\ShadowTheHedgehogDiskGC_Extract\RW37SDK\_mined\rw_guest_naming_candidates.csv")
    args = ap.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    root = args.project
    dtk_root = args.dtk_project or os.path.join(os.path.dirname(os.path.abspath(root)), "pal")

    image = dolmod.load_file(os.path.join(root, "main.dol"))
    funcs, objects = load_symbols(os.path.join(root, "config", "symbols.txt"))
    addr_of = {n: a for a, n, _, _ in funcs}
    starts = [a for a, _, _, _ in funcs]
    fn_at = {a: (n, s, sec) for a, n, s, sec in funcs}
    obj_addr = {n: v[0] for n, v in objects.items()}

    layer: dict[int, tuple[str, str, str]] = {}   # address -> (layer, rule, evidence)

    def give(a, lay, rule, ev):
        if a in fn_at and a not in layer:
            layer[a] = (lay, rule, ev)

    # ---- A1 dtk names -----------------------------------------------------------------------------------------
    dtk_funcs, _ = load_symbols(os.path.join(dtk_root, "config", "symbols.txt"))
    for a, n, _, _ in dtk_funcs:
        if re.fullmatch(r"(fn|lbl|dtor)_[0-9A-F]{8}", n) or n.startswith("@"):
            continue
        if n == "main":
            give(a, "game", "A1 name", "main (the game's entry point)")
        elif RUNTIME_NAMES.match(n):
            give(a, "runtime", "A1 name", f"dtk name {n}")
        elif MSL_NAMES.match(n):
            give(a, "msl", "A1 name", f"dtk name {n}")
        elif SDK_PREFIX.match(n):
            give(a, "sdk", "A1 name", f"dtk name {n}")

    # ---- A2 RenderWare anchors ------------------------------------------------------------------------------
    if args.rw_candidates and os.path.exists(args.rw_candidates):
        with open(args.rw_candidates, encoding="utf-8") as f:
            for row in csv.DictReader(f):
                if row.get("Confidence") == "STRONG":
                    give(int(row["GuestAddr"], 16), "rw", "A2 rw",
                         f"RW37SDK static-library anchor {row['ProposedSymbol']} ({row['Module']})")

    # ---- A3 classes -----------------------------------------------------------------------------------------
    sdata = next((s for s in image.sections if s.address <= min(v[0] for v in objects.values() if v[2] == ".sdata")
                  < s.address_end), None)
    table = corrmod.rtti_classes(image, (sdata.address, sdata.address_end), {v[0] for v in objects.values()},
                                 {a: s for a, _, s, _ in funcs})

    def class_layer(cls: str) -> str:
        if cls.startswith("boost::"):
            return "boost"
        if cls.startswith("std::"):
            return "msl"
        return "game"

    # A template instance (boost::detail::sp_counted_base_impl<T, D>, Chao::CSD::RCPtr<T>) is its template's code
    # but CodeWarrior emits it in the unit that instantiates it: it has a layer, yet its position says nothing about
    # its neighbours, so it seeds no S1 span, though it ends one (found 2026-11-01: two sp_counted_base_impl<T>
    # constructors in one game file spread "boost" over the game functions between them, once names.json gave A3
    # their class as a field).
    placed_elsewhere = set()

    def emitted_in_users(cls):
        """Template instances, Boost (header-only) and std classes (their inline members): CodeWarrior emits their
        code in the units that use them (std::bad_alloc's constructor sits among the camera code at 0x8000D4BC)."""
        return "<" in cls or cls.startswith(("boost::", "std::"))

    ancestors = {c: set(info["bases"]) | {c} for c, info in table.items()}
    for f, hs in corrmod.slot_holders(table).items():
        # every vtable slot (corrections.slot_holders): a secondary vtable's body is its subobject class's code, a
        # thunk the code of the class whose table holds it
        classes = corrmod.body_classes(hs) if any(h[0] != "thunk" for h in hs) else {h[1] for h in hs}
        if not classes:
            continue
        lays = {class_layer(c) for c in classes}
        if len(lays) == 1:
            give(f, lays.pop(), "A3 class", f"vtable slot of {sorted(classes)[0]}"
                 + (f" (+{len(classes) - 1} classes)" if len(classes) > 1 else ""))
            # where the code sits follows the class it belongs to (the one all holders descend from); a derived
            # template that only inherits the body does not emit it. No such class: any holder decides.
            owner = [c for c in classes if all(c in ancestors.get(o, {o}) for o in classes)]
            if any(emitted_in_users(c) for c in (owner if len(owner) == 1 else classes)):
                placed_elsewhere.add(f)
    names_path = os.path.join(root, "names.json")
    if os.path.exists(names_path):
        with open(names_path, encoding="utf-8") as f:
            nj = json.load(f)["names"]
        for a_hex, row in nj.items():
            # the class names.py recorded as a field (until 2026-11-01 parsed out of the evidence prose). A
            # constructor / destructor (N6 / N7) or a body word for word Boost's (N16) IS the class's code; a
            # function that only constructs a C into an object (N2) or uses one (class_uses) is the class's code
            # only for a game class - using a Boost or std object does not make code Boost or std (Astra answer 6)
            if not row.get("class"):
                continue
            implements = row["rule"] in ("N6 ctor", "N7 dtor", "N16 boost shared_ptr")
            if implements or row["rule"] == "N2 construct" and class_layer(row["class"]) == "game":
                give(int(a_hex, 16), class_layer(row["class"]), "A3 class",
                     {"N7 dtor": "destroys ", "N16 boost shared_ptr": "Boost code of "}.get(row["rule"], "constructs ")
                     + row["class"])
                if emitted_in_users(row["class"]):
                    placed_elsewhere.add(int(a_hex, 16))
        with open(names_path, encoding="utf-8") as f:
            for a_hex, row in json.load(f).get("class_uses", {}).items():     # unnamed users of a class's vtable
                if class_layer(row["class"]) == "game":
                    give(int(a_hex, 16), "game", "A3 class", row["evidence"])
                    if emitted_in_users(row["class"]):
                        placed_elsewhere.add(int(a_hex, 16))

    # ---- references from the split objects (calls, pointers, data) ------------------------------------------
    obj_dir = os.path.join(root, "build", "obj")
    calls = collections.defaultdict(collections.Counter)   # caller -> callee -> sites
    fptrs = collections.defaultdict(set)                    # function -> functions whose address it takes
    datarefs = collections.defaultdict(set)                 # function -> data symbol names
    for name in os.listdir(obj_dir):
        if not name.endswith(".o"):
            continue
        o = elfobj.read(os.path.join(obj_dir, name))
        sym_addr = {}
        for s in o.symbols:
            if s.type == elfobj.STT_FUNC and s.defined and s.name in addr_of:
                sym_addr[(s.shndx, s.value)] = addr_of[s.name]
        fstarts = sorted(sym_addr)
        for shndx, rows in o.relocs.items():
            sec_fns = sorted((v, a) for (sh, v), a in sym_addr.items() if sh == shndx)
            if not sec_fns:
                continue
            offs = [v for v, _ in sec_fns]
            for r in rows:
                i = bisect.bisect_right(offs, r.offset) - 1
                if i < 0:
                    continue
                f = sec_fns[i][1]
                tgt = r.symbol.name
                if tgt in addr_of:
                    if r.type in (10, 11):
                        calls[f][addr_of[tgt]] += 1
                    else:
                        fptrs[f].add(addr_of[tgt])
                elif tgt in obj_addr:
                    datarefs[f].add(tgt)

    # ---- A4 strings -----------------------------------------------------------------------------------------
    def cstring(addr):
        sec = image.section_at_address(addr)
        if sec is None:
            return None
        off = sec.file_offset + addr - sec.address
        end = image.raw.find(b"\0", off, min(off + 200, sec.file_end))
        if end <= off:
            return None
        try:
            return image.raw[off:end].decode("ascii")
        except UnicodeDecodeError:
            return None

    # A string pool (one object, many literals) is reached as its address plus an offset: reading at the object
    # gave every user the pool's FIRST string. Pools come from dolrecon.strrefs, whose per-use strings stand in for
    # them; a function whose strings point at two different libraries gets no A4 layer (Astra answer 5).
    pool_text, pooled, pool_objects = strrefs.project_refs(root)
    seen = collections.defaultdict(dict)                  # function -> {layer: first text}

    def classify(f, text):
        lay = ("cri" if CRI_TEXT.search(text) else "runtime" if RUNTIME_TEXT.search(text) else
               "sdk" if SDK_TEXT.search(text) else None)
        if lay:
            seen[f].setdefault(lay, text)

    for f, refs in datarefs.items():
        for t in sorted(refs):
            v = objects[t]
            if t in pool_objects:
                continue
            text = cstring(v[0]) if v[2] in (".rodata", ".data", ".sdata", ".sdata2") else None
            if text:
                classify(f, text)
    for f, rs in pooled.items():
        for _, a in rs:
            classify(f, pool_text[a])
    for f, lays in sorted(seen.items()):
        if len(lays) == 1:
            (lay, text), = lays.items()
            give(f, lay, "A4 string", f"references {text[:60]!r}")

    # ---- S1 between anchors of one layer --------------------------------------------------------------------
    text_secs = sorted((s.address, s.address_end) for s in image.sections if s.kind == "text")

    def section_of(a):
        for lo, hi in text_secs:
            if lo <= a < hi:
                return lo
        return None

    anchored = sorted(layer)
    for i in range(len(anchored) - 1):
        lo, hi = anchored[i], anchored[i + 1]
        la, lb = layer[lo][0], layer[hi][0]
        # an instance emitted in its user's unit seeds no span but still ends one
        if la != lb or section_of(lo) != section_of(hi) or lo in placed_elsewhere or hi in placed_elsewhere:
            continue
        j = bisect.bisect_right(starts, lo)
        while j < len(starts) and starts[j] < hi:
            give(starts[j], la, "S1 between", f"between {la} anchors {fn_at[lo][0]} and {fn_at[hi][0]}")
            j += 1

    # Between a game anchor and a Boost anchor (the S2 intervals below): game code or a Boost template instantiated
    # there. G1 must not decide "game" inside one - being called by game code is what an instance is (Astra 6).
    a_anchors = sorted(a for a in layer if layer[a][1].startswith("A"))      # Boost instances included: the signal
    mixed = []
    for i in range(len(a_anchors) - 1):
        lo, hi = a_anchors[i], a_anchors[i + 1]
        if {layer[lo][0], layer[hi][0]} == {"game", "boost"} and section_of(lo) == section_of(hi):
            mixed.append((lo, hi))
    in_mixed = set()
    for lo, hi in mixed:
        in_mixed.update(starts[bisect.bisect_right(starts, lo):bisect.bisect_left(starts, hi)])

    # ---- G1 call graph --------------------------------------------------------------------------------------
    # Only CALLERS identify a function's layer (a library's internal helpers are called by that library; calling
    # memcpy says nothing about who you are). The callees must be ones that layer can call.
    callers = collections.defaultdict(set)
    for f, cs in calls.items():
        for g in cs:
            if g != f:
                callers[g].add(f)
    for _ in range(8):
        added = 0
        for a in starts:
            if a in layer:
                continue
            lays = {layer[n][0] for n in callers.get(a, ()) if n in layer}
            if len(lays) != 1:
                continue
            lay = lays.pop()
            if lay == "game" and a in in_mixed:
                continue
            if lay != "game" and "game" in MAY_CALL[lay]:
                # Boost templates instantiated for a game type call that type's code (checked_deleter -> its
                # destructor, bind -> its member function): being called only from them makes nothing Boost
                # (found 2026-11-01, when the sp_counted_base_impl<T> constructors became Boost anchors: 1,314
                # game functions followed them).
                continue
            callee_lays = {layer[g][0] for g in calls.get(a, {}) if g in layer}
            if not callee_lays <= MAY_CALL[lay]:
                continue
            n_res = sum(1 for n in callers[a] if n in layer)
            give(a, lay, "G1 graph", f"all {n_res} resolved callers are {lay}")
            added += 1
        if not added:
            break

    # ---- S2 game unit ---------------------------------------------------------------------------------------
    # Between a game anchor and a Boost anchor: inside the game's own object files (game code or a Boost template
    # instantiated there). Which of the two is not decidable from link order, so it gets its own layer.
    for lo, hi in mixed:
        j = bisect.bisect_right(starts, lo)
        while j < len(starts) and starts[j] < hi:
            give(starts[j], "game-unit", "S2 game unit", f"between {fn_at[lo][0]} ({layer[lo][0]}) and "
                 f"{fn_at[hi][0]} ({layer[hi][0]}): in the game's object files")
            j += 1

    # ---- environment scan -----------------------------------------------------------------------------------
    env = {a: decode_env(image, a, s) for a, (n, s, sec) in fn_at.items() if s}

    # ---- SDK sub-library: the nearest preceding dtk SDK name in link order (archives link whole) ------------
    sdk_anchor = sorted((a, n) for a, n, _, _ in dtk_funcs if layer.get(a, ("",))[0] == "sdk" and a in fn_at
                        and layer[a][1] == "A1 name")
    sdk_starts = [a for a, _ in sdk_anchor]

    def lib_of_name(n):
        m = re.match(r"_*(PPC|OS|DC|IC|LC|L2|DB|DVD|VI|PAD|SI|EXI|AI|ARQ|AR|AXFX|AX|MIX|SP|DSP|CARD|GX|GD|THP|TRK)",
                     n)
        if m:
            return {"PPC": "OS", "DC": "OS", "IC": "OS", "LC": "OS", "L2": "OS", "DB": "OS"}.get(m[1], m[1])
        if re.match(r"(Decrementer|ConfigBus|ExternalInterrupt|SelectThread|SystemCall)", n):
            return "OS"
        if re.match(r"(Alarm|cbFor|state[A-Z]|ErrorCode2Num|__fstLoad|cb$)", n):
            return "DVD"
        if re.match(r"(InitMetroTRK|EnableMetroTRK|MetroTRK|InitializeProgramEndTrap|ReserveEXI2Port|"
                    r"UnreserveEXI2Port|gdev_|ddh_|udp_|usr_)", n):
            return "TRK"
        if re.match(r"(AMC_|Hu_|SetExiInterruptMask|Unlocked)", n):
            return "EXI"
        if re.match(r"GetTypeCallback", n):
            return "SI"
        return "?"

    def sdk_lib(a):
        """The SDK library of a function: its own dtk name, or the library of the dtk-named functions on BOTH
        sides in link order (archives link whole). Otherwise 'A..B' (between libraries)."""
        if lay_of(a) != "sdk":
            return ""
        i = bisect.bisect_right(sdk_starts, a) - 1
        if i >= 0 and sdk_starts[i] == a:
            return lib_of_name(sdk_anchor[i][1])
        lo = lib_of_name(sdk_anchor[i][1]) if i >= 0 else "?"
        hi = lib_of_name(sdk_anchor[i + 1][1]) if i + 1 < len(sdk_anchor) else "?"
        return lo if lo == hi else f"{lo}..{hi}"

    # ---- outputs --------------------------------------------------------------------------------------------
    def lay_of(a):
        return layer.get(a, ("unresolved", "", ""))[0]

    rows = []
    for a, (n, s, sec) in sorted(fn_at.items()):
        lay, rule, ev = layer.get(a, ("unresolved", "", ""))
        e = env.get(a, collections.Counter())
        out_calls = collections.Counter()
        for g, k in calls.get(a, {}).items():
            out_calls[lay_of(g)] += k
        rows.append({"address": f"0x{a:08X}", "name": n, "size": s, "layer": lay, "sdk_lib": sdk_lib(a),
                     "rule": rule, "evidence": ev,
                     "env": " ".join(f"{k}:{v}" for k, v in sorted(e.items()) if k in ENV_FLAGS),
                     "isa": " ".join(f"{k}:{v}" for k, v in sorted(e.items()) if k not in ENV_FLAGS),
                     "calls_sdk": out_calls["sdk"], "calls_rw": out_calls["rw"], "calls_cri": out_calls["cri"],
                     "calls_msl": out_calls["msl"], "calls_runtime": out_calls["runtime"]})
    with open(os.path.join(args.out, "functions.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    # boundary: game -> other layer
    boundary = collections.defaultdict(lambda: {"sites": 0, "callers": set()})
    for f, cs in calls.items():
        if lay_of(f) != "game":
            continue
        for g, k in cs.items():
            lg = lay_of(g)
            if lg in ("game", "game-unit", "boost"):
                continue
            b = boundary[g]
            b["sites"] += k
            b["callers"].add(f)
    callbacks = collections.defaultdict(set)   # game function -> non-game functions that take its address
    for f, ts in fptrs.items():
        if lay_of(f) in ("game", "game-unit", "boost", "unresolved"):
            continue
        for t in ts:
            if lay_of(t) in ("game", "game-unit"):
                callbacks[t].add(f)
    with open(os.path.join(args.out, "boundary.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["callee", "address", "layer", "sdk_lib", "game_call_sites", "game_callers"])
        for g, b in sorted(boundary.items(), key=lambda kv: (lay_of(kv[0]), -kv[1]["sites"])):
            w.writerow([fn_at[g][0], f"0x{g:08X}", lay_of(g), sdk_lib(g), b["sites"], len(b["callers"])])

    by_layer = collections.Counter(r["layer"] for r in rows)
    bytes_layer = collections.Counter()
    for r in rows:
        bytes_layer[r["layer"]] += r["size"]
    by_rule = collections.Counter((r["layer"], r["rule"]) for r in rows)
    game = [r for r in rows if r["layer"] in ("game", "game-unit")]
    game_env = [r for r in game if r["env"]]
    game_sdk = [r for r in game if r["calls_sdk"]]
    env_kinds = collections.Counter()
    for r in game_env:
        for kv in r["env"].split():
            env_kinds[kv.split(":")[0]] += 1
    pure = [r for r in game if not r["env"] and not r["calls_sdk"] and not r["calls_cri"] and not r["calls_rw"]]
    summary = {
        "functions": len(rows),
        "layers": {k: {"functions": by_layer[k], "bytes": bytes_layer[k]} for k in LAYERS + ["unresolved"]},
        "rules": {f"{k[0]} {k[1]}": v for k, v in sorted(by_rule.items())},
        "game": {
            "functions": len(game),
            "touch_environment_directly": len(game_env),
            "environment_kinds": dict(env_kinds),
            "call_sdk_directly": len(game_sdk),
            "call_cri": sum(1 for r in game if r["calls_cri"]),
            "call_rw": sum(1 for r in game if r["calls_rw"]),
            "pure_logic": len(pure),
            "pure_logic_bytes": sum(r["size"] for r in pure),
            "uses_paired_singles": sum(1 for r in game if r["isa"]),
        },
        "boundary_functions": {lay: sum(1 for g in boundary if lay_of(g) == lay) for lay in LAYERS[3:] + ["unresolved"]},
        "boundary_sites": {lay: sum(b["sites"] for g, b in boundary.items() if lay_of(g) == lay)
                           for lay in LAYERS[3:] + ["unresolved"]},
        "game_callbacks_taken_by_libraries": len(callbacks),
    }
    with open(os.path.join(args.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    with open(os.path.join(args.out, "game_platform_glue.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["address", "name", "size", "env", "sdk_callees"])
        for r in game:
            if r["env"] or r["calls_sdk"]:
                a = int(r["address"], 16)
                sdk = sorted({fn_at[g][0] for g in calls.get(a, {}) if lay_of(g) == "sdk"})
                w.writerow([r["address"], r["name"], r["size"], r["env"], " ".join(sdk)])
    with open(os.path.join(args.out, "callbacks.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["game_function", "address", "taken_by"])
        for t, fs in sorted(callbacks.items()):
            w.writerow([fn_at[t][0], f"0x{t:08X}", " ".join(sorted(fn_at[x][0] for x in fs))])
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
