"""The host instruction-set contract of an image is enforced before any code that relies on it (L0r0-02).

SHADOW_X64_ARCH (CMakeLists.txt, default AVX2) compiles the generated recomp shards, the runtime and every TU
that includes runtime/include/gekko with /arch:<level>. Those images execute LZCNT (ppc_ops.h cntlzw), FMA3
(fused_multiply_add), BMI1/BMI2 and AVX/AVX2 unconditionally - from the first C++ static initializer onwards.
On a processor or operating system that lacks one of them the process dies with an unexplained
STATUS_ILLEGAL_INSTRUCTION, and LZCNT on a pre-LZCNT processor even decodes as BSR and returns a different
count instead of faulting. The images must therefore check the host (CPUID + XGETBV) and refuse with a
diagnostic BEFORE the first instruction compiled for the level runs.

Lanes, every one must pass for every checked image (the map written by the linker is required):

  A  order: from the PE image and its linker map, every initializer that runs before the host ISA gate -
     TLS callbacks, thread_local initializers (.CRT$XD*), the CRT entry point, the C initializers
     (.CRT$XI*) that precede the gate - belongs to the C/C++ runtime libraries, and the gate is a
     C initializer, so it runs before every C++ initializer (.CRT$XC*) and before main.
  B  gate code: every function of the gate object in the linked image is x86-64 baseline code (no
     instruction needs a CPUID feature bit; it executes CPUID and XGETBV), and it transfers control only
     to itself, to imports and to the C runtime - never to a COMDAT that an /arch TU may have supplied.
  C  coverage: the instruction set extensions that the image's contract code (runtime, generated shards,
     the program's own objects) actually contains are a subset of what the gate requires - measured over
     the whole disassembly, so a compiler or source change that starts using a new extension fails here.
  D  refusal: the real image, run with SHADOW_HOST_ISA_MASK=<feature> (the gate's diagnostic that treats a
     feature as absent; it can only remove features), refuses with exit code 29 and a message naming the
     feature, and prints nothing from main, exactly for the features it declares required; the unmasked
     image passes the gate on this host and reaches main (--help).
  E  presence: every built executable whose compile groups use a non-default host ISA model for
     runtime/include carries the gate.
"""
import argparse
import bisect
import collections
import os
import pathlib
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_host_isa_consistency as consistency  # noqa: E402  (lane E reuses its codemodel reader)

FEATURES = ("SSE3", "SSSE3", "SSE4.1", "SSE4.2", "POPCNT", "CMPXCHG16B", "LAHF-SAHF", "OSXSAVE", "YMM-STATE",
            "AVX", "AVX2", "BMI1", "BMI2", "F16C", "FMA3", "LZCNT", "MOVBE", "ZMM-STATE", "AVX512F", "AVX512BW",
            "AVX512CD", "AVX512DQ", "AVX512VL")
REFUSAL_EXIT = 29
MASK_ENV = "SHADOW_HOST_ISA_MASK"
GATE_PREFIX = "host ISA gate:"
GATE_OBJECT = "host_isa_gate.obj"
GATE_POINTER = "shadow_host_isa_gate_initializer"
CRT_LIBRARIES = {"libcmt", "libcmtd", "msvcrt", "msvcrtd", "libucrt", "libucrtd", "ucrt", "ucrtd",
                 "libvcruntime", "libvcruntimed", "vcruntime", "vcruntimed", "libcpmt", "libcpmtd",
                 "msvcprt", "msvcprtd", "libconcrt", "libconcrtd"}
CRT_ENTRY_POINTS = {"mainCRTStartup", "wmainCRTStartup", "WinMainCRTStartup", "wWinMainCRTStartup"}
DEFAULT_CONTRACT_LIBRARIES = ("shadow_runtime", "shadow_runtime_reference", "shadow_recomp")
CREATE_NO_WINDOW = 0x08000000


class Failure(Exception):
    pass


# ------------------------------------------------------------------ instruction classification
_YMM_STATE = ("AVX", "OSXSAVE", "YMM-STATE")
FAMILY_FEATURES = {
    "SSE3": ("SSE3",), "SSSE3": ("SSSE3",), "SSE4.1": ("SSE4.1",), "SSE4.2": ("SSE4.2",), "POPCNT": ("POPCNT",),
    "CMPXCHG16B": ("CMPXCHG16B",), "LAHF-SAHF": ("LAHF-SAHF",), "AVX": _YMM_STATE, "AVX2": ("AVX2",) + _YMM_STATE,
    "FMA": ("FMA3",) + _YMM_STATE, "F16C": ("F16C",) + _YMM_STATE, "BMI1": ("BMI1",), "BMI2": ("BMI2",),
    "LZCNT": ("LZCNT",), "MOVBE": ("MOVBE",), "AVX512": ("AVX512F", "ZMM-STATE") + _YMM_STATE,
}
_LEGACY = {}
for _family, _names in {
    "SSE3": "haddpd haddps hsubpd hsubps lddqu movddup movshdup movsldup fisttp",
    "SSSE3": "pabsb pabsw pabsd palignr phaddd phaddsw phaddw phsubd phsubsw phsubw pmaddubsw pmulhrsw pshufb "
             "psignb psignd psignw",
    "SSE4.1": "blendpd blendps blendvpd blendvps dppd dpps extractps insertps mpsadbw packusdw pblendvb pblendw "
              "pcmpeqq pextrb pextrd pextrq pinsrb pinsrd pinsrq phminposuw pmaxsb pmaxsd pmaxud pmaxuw pminsb "
              "pminsd pminud pminuw pmuldq pmulld ptest roundpd roundps roundsd roundss movntdqa",
    "SSE4.2": "crc32 pcmpestri pcmpestrm pcmpistri pcmpistrm pcmpgtq",
    "POPCNT": "popcnt", "CMPXCHG16B": "cmpxchg16b", "LAHF-SAHF": "lahf sahf",
    "BMI1": "tzcnt andn blsr blsi blsmsk bextr", "BMI2": "shlx shrx sarx rorx bzhi pdep pext mulx",
    "LZCNT": "lzcnt", "MOVBE": "movbe",
}.items():
    for _name in _names.split():
        _LEGACY[_name] = _family
# Extensions the gate has no vocabulary for: finding one in contract code is itself a failure.
_UNMODELLED = set(("aesenc aesdec aesenclast aesdeclast aesimc aeskeygenassist pclmulqdq rdrand rdseed adcx adox "
                   "sha1msg1 sha1msg2 sha1nexte sha1rnds4 sha256msg1 sha256msg2 sha256rnds2 xsave xsaveopt xsavec "
                   "xsaves xrstor xrstors gf2p8affineqb gf2p8affineinvqb gf2p8mulb").split())
_NOT_VEX = {"verr", "verw", "vmcall", "vmlaunch", "vmresume", "vmxoff", "vmxon", "vmread", "vmwrite", "vmfunc"}
_AVX2_ONLY = set(("vpbroadcastb vpbroadcastw vpbroadcastd vpbroadcastq vbroadcasti128 vinserti128 vextracti128 "
                  "vpermq vpermd vperm2i128 vpermps vpermpd vpblendd vpmaskmovd vpmaskmovq vpsllvd vpsllvq vpsrlvd "
                  "vpsrlvq vpsravd vgatherdps vgatherqps vgatherdpd vgatherqpd vpgatherdd vpgatherqd vpgatherdq "
                  "vpgatherqq").split())
_EVEX_REGISTER = re.compile(r"\b(zmm\d+|[xy]mm(1[6-9]|2\d|3[01])|k[1-7])\b")


def classify(mnemonic, operands):
    """The CPUID-gated extension an instruction needs (Intel SDM Vol. 2 per-instruction CPUID flag), 'GATE' for
    the detection instructions themselves, None for x86-64 baseline."""
    if mnemonic in ("cpuid", "xgetbv"):
        return "GATE"
    if mnemonic in _LEGACY:
        return _LEGACY[mnemonic]
    if mnemonic.startswith(("pmovsx", "pmovzx")):
        return "SSE4.1"
    if mnemonic in _UNMODELLED:
        return "UNMODELLED:" + mnemonic
    if not mnemonic.startswith("v") or mnemonic in _NOT_VEX:
        return None
    if mnemonic[1:] in _UNMODELLED:
        return "UNMODELLED:" + mnemonic
    if _EVEX_REGISTER.search(operands):
        return "AVX512"
    if mnemonic.startswith(("vfmadd", "vfmsub", "vfnmadd", "vfnmsub")):
        return "FMA"
    if mnemonic in ("vcvtph2ps", "vcvtps2ph"):
        return "F16C"
    if mnemonic in _AVX2_ONLY or (mnemonic.startswith("vp") and "ymm" in operands):
        return "AVX2"
    return "AVX"


# ------------------------------------------------------------------ PE image + linker map
class Image:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise Failure(f"{path}: not a PE image")
        machine, sections, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", self.data, pe + 4)
        opt = pe + 24
        if struct.unpack_from("<H", self.data, opt)[0] != 0x20B:
            raise Failure(f"{path}: not PE32+")
        self.entry_rva = struct.unpack_from("<I", self.data, opt + 16)[0]
        self.base = struct.unpack_from("<Q", self.data, opt + 24)[0]
        tls_rva, tls_size = struct.unpack_from("<II", self.data, opt + 112 + 9 * 8)
        self.sections = []
        for i in range(sections):
            s = opt + optional_size + 40 * i
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", self.data, s)
            self.sections.append((name.rstrip(b"\0").decode("ascii", "replace"), va, vsize, rptr, rsize))
        self.tls = None
        if tls_rva:
            self.tls = struct.unpack_from("<QQQQ", self.data, self.rva_offset(tls_rva))

    def rva_offset(self, rva):
        for _, va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                if rva - va >= rsize:
                    return None  # zero-fill
                return rptr + rva - va
        raise Failure(f"{self.path}: RVA {rva:#x} is in no section")

    def qword_at_va(self, va):
        off = self.rva_offset(va - self.base)
        return 0 if off is None else struct.unpack_from("<Q", self.data, off)[0]

    def tls_callbacks(self):
        if not self.tls:
            return []
        out, va = [], self.tls[3]
        while va:
            target = self.qword_at_va(va)
            if not target:
                break
            out.append(target)
            va += 8
        return out


_MAP_SECTION = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+([0-9a-f]{8})H\s+(\S+)\s+(CODE|DATA)\s*$", re.I)
_MAP_SYMBOL = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)\s*$", re.I)


class LinkerMap:
    def __init__(self, path):
        self.sections, symbols = [], []
        self.entry = None
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _MAP_SECTION.match(line)
            if m:
                self.sections.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
                continue
            m = _MAP_SYMBOL.match(line)
            if m:
                symbols.append((int(m.group(4), 16), m.group(3), m.group(5), int(m.group(1), 16)))
        symbols.sort()
        self.symbols = symbols
        self.addresses = [s[0] for s in symbols]
        self.by_name = collections.defaultdict(list)
        for s in symbols:
            self.by_name[s[1]].append(s)

    def owner(self, va):
        """(symbol, owner 'lib:object' or 'object', offset) of the symbol at or before va."""
        i = bisect.bisect_right(self.addresses, va) - 1
        if i < 0:
            return ("?", "?", 0)
        # Prefer a non-$LN name among aliases at the same address.
        j = i
        while j > 0 and self.addresses[j - 1] == self.addresses[i]:
            j -= 1
        candidates = [s for s in self.symbols[j:i + 1] if not s[1].startswith("$LN")] or [self.symbols[i]]
        s = candidates[0]
        return (s[1], s[2], va - s[0])


def library_of(owner):
    return owner.split(":", 1)[0].lower() if ":" in owner else ""


def is_crt(owner):
    return library_of(owner) in CRT_LIBRARIES


# ------------------------------------------------------------------ lane A: initializer order
def crt_table_entries(image, lmap, prefix):
    """[(section name, pointer va, target va)] for every non-null pointer in .CRT$<prefix>* in link order."""
    out = []
    pe_sections = image.sections
    for index, offset, length, name in lmap.sections:
        if not name.startswith(".CRT$" + prefix):
            continue
        va = image.base + pe_sections[index - 1][1] + offset
        for slot in range(0, length, 8):
            target = image.qword_at_va(va + slot)
            if target:
                out.append((name, va + slot, target))
    return out


def lane_a(name, image, lmap):
    failures, order = [], []
    for callback in image.tls_callbacks():
        sym, owner, off = lmap.owner(callback)
        order.append(("TLS callback", sym, owner))
        if not is_crt(owner) or off:
            failures.append(f"{name}: TLS callback {sym} [{owner}] runs before the entry point and is not C runtime code")
    for section, _, target in crt_table_entries(image, lmap, "XD"):
        sym, owner, _ = lmap.owner(target)
        order.append((section, sym, owner))
        if not is_crt(owner):
            failures.append(f"{name}: thread_local initializer {sym} [{owner}] ({section}) runs from a TLS callback, "
                            f"before the host ISA gate")
    sym, owner, off = lmap.owner(image.base + image.entry_rva)
    order.append(("entry point", sym, owner))
    if sym not in CRT_ENTRY_POINTS or not is_crt(owner) or off:
        failures.append(f"{name}: entry point is {sym}+{off:#x} [{owner}], not a C runtime startup routine")
    gate_seen = None
    for section, pointer, target in crt_table_entries(image, lmap, "XI"):
        sym, owner, off = lmap.owner(target)
        pointer_sym = lmap.owner(pointer)[0]
        order.append((section, sym, owner))
        if owner == GATE_OBJECT or owner.endswith(":" + GATE_OBJECT):
            if pointer_sym != GATE_POINTER:
                failures.append(f"{name}: gate function {sym} is referenced from {pointer_sym}, expected {GATE_POINTER}")
            gate_seen = (section, sym)
            break
        if not is_crt(owner):
            failures.append(f"{name}: C initializer {sym} [{owner}] ({section}) runs before the host ISA gate")
    if gate_seen is None:
        failures.append(f"{name}: no host ISA gate among the C initializers (.CRT$XI*): nothing checks CPUID/XGETBV "
                        f"before the /arch code runs")
        late = [(s, lmap.owner(t)) for s, _, t in crt_table_entries(image, lmap, "XC")
                if lmap.owner(t)[1].endswith(GATE_OBJECT)]
        if late:
            failures.append(f"{name}: the gate is a C++ initializer ({late[0][0]}), which may run after /arch initializers")
    else:
        first_cxx = next(((s, lmap.owner(t)) for s, _, t in crt_table_entries(image, lmap, "XC")
                          if not is_crt(lmap.owner(t)[1])), None)
        detail = f"; first project C++ initializer {first_cxx[1][0]} [{first_cxx[1][1]}] ({first_cxx[0]}) runs after it" \
            if first_cxx else ""
        if not failures:
            print(f"PASS A {name}: {len(order) - 1} CRT-owned step(s) precede the gate {gate_seen[1]} "
                  f"({gate_seen[0]}){detail}")
    return failures


# ------------------------------------------------------------------ disassembly
_DIS_LINE = re.compile(r"^\s+([0-9A-F]{16}):\s+(\S+)\s*(.*)$")


def dumpbin_disasm(dumpbin, image_path, first=None, last=None):
    command = [dumpbin, "/nologo", "/disasm:nobytes"]
    if first is not None:
        command.append(f"/range:0x{first:X},0x{last:X}")
    command.append(str(image_path))
    return subprocess.Popen(command, stdout=subprocess.PIPE, text=True, errors="replace", bufsize=1 << 22)


def lane_b(name, image_path, lmap, dumpbin):
    failures = []
    gate_symbols = [s for s in lmap.symbols if s[3] == 1 and (s[2] == GATE_OBJECT or s[2].endswith(":" + GATE_OBJECT))]
    if not gate_symbols:
        return [f"{name}: the linked image contains no code from {GATE_OBJECT}"]
    ranges = {}
    for va, sym, _, _ in gate_symbols:
        i = bisect.bisect_right(lmap.addresses, va)
        while i < len(lmap.addresses) and lmap.addresses[i] == va:
            i += 1
        end = lmap.addresses[i] if i < len(lmap.addresses) else va + 0x1000
        ranges[va] = (end, sym)
    seen = collections.Counter()
    transfers = 0
    for va, (end, sym) in sorted(ranges.items()):
        proc = dumpbin_disasm(dumpbin, image_path, va, end - 1)
        for line in proc.stdout:
            m = _DIS_LINE.match(line)
            if not m:
                continue
            mnemonic, operands = m.group(2), m.group(3)
            kind = classify(mnemonic, operands)
            if kind == "GATE":
                seen[mnemonic] += 1
            elif kind is not None:
                failures.append(f"{name}: gate function {sym} executes {mnemonic} {operands} ({kind}) at {m.group(1)}")
            if mnemonic == "int" and operands.strip() == "3":
                continue
            if mnemonic.startswith("j") or mnemonic == "call":
                target = re.search(r"\b([0-9A-F]{16})h?\b", operands)
                if not target:
                    continue
                t = int(target.group(1), 16)
                if va <= t < end or any(a <= t < e for a, (e, _) in ranges.items()):
                    continue
                transfers += 1
                if "qword ptr" in operands:
                    tsym, towner, _ = lmap.owner(t)
                    if not tsym.startswith("__imp_"):
                        failures.append(f"{name}: gate function {sym} calls through {tsym} [{towner}], not an import")
                    continue
                tsym, towner, _ = lmap.owner(t)
                if not is_crt(towner) and not towner.lower().endswith(".dll"):
                    failures.append(f"{name}: gate function {sym} transfers to {tsym} [{towner}] at {m.group(1)}; "
                                    f"the gate may only use its own code, imports and the C runtime")
        proc.wait()
    for required in ("cpuid", "xgetbv"):
        if not seen[required]:
            failures.append(f"{name}: the gate object never executes {required}")
    if not failures:
        print(f"PASS B {name}: {len(ranges)} gate function(s) are x86-64 baseline, {seen['cpuid']} cpuid / "
              f"{seen['xgetbv']} xgetbv, {transfers} outward transfer(s) all to imports or the C runtime")
    return failures


# ------------------------------------------------------------------ lane D: refusal on the real image
def run_image(image_path, work, mask):
    env = dict(os.environ)
    env.pop(MASK_ENV, None)
    if mask is not None:
        env[MASK_ENV] = mask
    work.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run([str(image_path), "--help"], cwd=work, env=env, capture_output=True, text=True,
                                errors="replace", timeout=60, creationflags=CREATE_NO_WINDOW)
    except subprocess.TimeoutExpired:
        return None, "", "timeout"
    return result.returncode, result.stdout, result.stderr


def gate_lines(stderr, key):
    for line in stderr.splitlines():
        if line.startswith(GATE_PREFIX) and line[len(GATE_PREFIX):].strip().startswith(key + ":"):
            return line[len(GATE_PREFIX):].strip()[len(key) + 1:].split()
    return None


def lane_d(name, image_path, work, arch):
    failures = []
    code, out, err = run_image(image_path, work, None)
    if code != 0 or "Usage:" not in out or GATE_PREFIX in err:
        failures.append(f"{name}: unmasked run did not pass the gate and reach main (--help): exit {code}\n"
                        f"stdout: {out[:400]}\nstderr: {err[:800]}")
    required_declared, refused = None, set()
    for feature in FEATURES:
        code, out, err = run_image(image_path, work, feature)
        if code == REFUSAL_EXIT:
            refused.add(feature)
            missing = gate_lines(err, "missing") or []
            required = gate_lines(err, "required") or []
            level = gate_lines(err, "level") or []
            if feature not in missing:
                failures.append(f"{name}: {MASK_ENV}={feature} refused without naming it as missing:\n{err[:800]}")
            if out.strip():
                failures.append(f"{name}: {MASK_ENV}={feature} refused but main already wrote stdout: {out[:200]}")
            if level != [arch]:
                failures.append(f"{name}: gate reports level {level}, the build contract is SHADOW_X64_ARCH={arch}")
            if required_declared is None:
                required_declared = set(required)
            elif set(required) != required_declared:
                failures.append(f"{name}: gate declared different required sets across runs")
        elif code == 0 and "Usage:" in out and GATE_PREFIX not in err:
            pass
        else:
            failures.append(f"{name}: {MASK_ENV}={feature}: neither refused (exit {REFUSAL_EXIT}) nor passed to main:"
                            f" exit {code}\nstdout: {out[:300]}\nstderr: {err[:800]}")
    if not refused:
        failures.append(f"{name}: no {MASK_ENV}=<feature> run was refused: the image never checks the host "
                        f"instruction set it was compiled for (SHADOW_X64_ARCH={arch})")
    elif required_declared is not None and refused != required_declared:
        failures.append(f"{name}: refused for {sorted(refused)} but declares required {sorted(required_declared)}")
    code, out, err = run_image(image_path, work, "NOT-A-FEATURE")
    if code != REFUSAL_EXIT or "NOT-A-FEATURE" not in err:
        failures.append(f"{name}: an unknown {MASK_ENV} token was not rejected: exit {code}\n{err[:400]}")
    if refused and refused != set():
        code, out, err = run_image(image_path, work, ",".join(sorted(refused)))
        missing = set(gate_lines(err, "missing") or [])
        if code != REFUSAL_EXIT or missing != refused:
            failures.append(f"{name}: masking every required feature reported missing {sorted(missing)}")
    if not failures:
        print(f"PASS D {name}: refuses (exit {REFUSAL_EXIT}, nothing from main) exactly for {len(refused)} required "
              f"feature(s) {' '.join(f for f in FEATURES if f in refused)}; unmasked it passes on this host")
    return failures, refused


# ------------------------------------------------------------------ lane C: coverage of the measured use
def _quick_candidate(mnemonic):
    return mnemonic[0] == "v" or mnemonic in _LEGACY or mnemonic in _UNMODELLED or \
        mnemonic.startswith(("pmovsx", "pmovzx"))


def lane_c(name, image_path, lmap, dumpbin, required, contract_libraries, jobs=8):
    """A linear sweep over .text decodes the switch jump tables MSVC x64 places after a function as code.
    Two consequences are handled exactly rather than filtered by instruction kind: (1) instructions under a
    $LN label are that table data; (2) the sweep leaves the table out of step, so the next function start can
    fall INSIDE a decoded 'instruction' - every such straddled function is disassembled again from its real
    start up to the next label, and those results replace the sweep's for that window."""
    failures = []
    text_starts = sorted({s[0] for s in lmap.symbols if s[3] == 1})
    function_starts = sorted({s[0] for s in lmap.symbols if s[3] == 1 and not s[1].startswith("$LN")})
    records, straddled, total, prev_va = [], [], 0, None
    proc = dumpbin_disasm(dumpbin, image_path)
    for line in proc.stdout:
        if len(line) < 22 or line[18] != ":":
            continue
        total += 1
        va = int(line[2:18], 16)
        if prev_va is not None:
            i = bisect.bisect_right(function_starts, prev_va)
            if i < len(function_starts) and function_starts[i] < va:
                straddled.append(function_starts[i])
                if records and records[-1][0] == prev_va:
                    records.pop()  # the straddling 'instruction' is table data spanning into code
        prev_va = va
        field = line[20:].split(None, 1)
        if not field or not _quick_candidate(field[0]):
            continue
        records.append((va, field[0], field[1] if len(field) > 1 else ""))
    proc.wait()
    if total < 1000:
        return [f"{name}: dumpbin produced only {total} instructions"]
    windows = []
    for start in straddled:
        i = bisect.bisect_right(text_starts, start)
        windows.append((start, text_starts[i] if i < len(text_starts) else start + 0x1000))
    window_starts = [w[0] for w in windows]

    def in_window(va):
        i = bisect.bisect_right(window_starts, va) - 1
        return i >= 0 and windows[i][0] <= va < windows[i][1]

    records = [r for r in records if not in_window(r[0])]

    def redisassemble(window):
        out = []
        p = dumpbin_disasm(dumpbin, image_path, window[0], window[1] - 1)
        for line in p.stdout:
            m = _DIS_LINE.match(line)
            if m and window[0] <= int(m.group(1), 16) < window[1] and _quick_candidate(m.group(2)):
                out.append((int(m.group(1), 16), m.group(2), m.group(3)))
        p.wait()
        return out

    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for chunk in pool.map(redisassemble, windows):
            records.extend(chunk)
    counts = collections.Counter()
    data_in_text = collections.Counter()
    samples = {}
    for va, mnemonic, operands in records:
        kind = classify(mnemonic, operands)
        if kind is None or kind == "GATE":
            continue
        sym, owner, _ = lmap.owner(va)
        if sym.startswith("$LN"):
            data_in_text[kind] += 1
            continue
        library = library_of(owner)
        in_contract = (library == "" and owner.lower().endswith(".obj")) or library in contract_libraries
        if not in_contract:
            continue
        counts[kind] += 1
        samples.setdefault(kind, f"{mnemonic} {operands.strip()} in {sym} [{owner}] at {va:016X}")
    print(f"INFO C {name}: {total} swept instructions, {len(windows)} function start(s) re-synchronised, "
          f"{sum(data_in_text.values())} extension decodes inside $LN jump-table data ignored")
    needed = set()
    for kind in counts:
        if kind.startswith("UNMODELLED:"):
            failures.append(f"{name}: contract code uses {kind[11:]} ({samples[kind]}), an extension the gate has "
                            f"no vocabulary for")
            continue
        uncovered = [f for f in FAMILY_FEATURES[kind] if f not in required]
        needed.update(FAMILY_FEATURES[kind])
        if uncovered:
            failures.append(f"{name}: contract code executes {counts[kind]} {kind} instruction(s), e.g. {samples[kind]}, "
                            f"but the gate does not require {' '.join(uncovered)}")
    census = ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
    if not failures:
        print(f"PASS C {name}: contract code uses {census}; all covered by the gate")
    else:
        print(f"INFO C {name}: {total} instructions; contract code uses {census}")
    return failures


# ------------------------------------------------------------------ lane E: gate present in every ISA image
def lane_e(build, source, config, cmake):
    failures = []
    try:
        _, targets = consistency.load_codemodel(build, config, cmake)
    except consistency.Failure as exc:
        return [f"lane E: {exc}"]
    include = (source / "runtime" / "include").resolve()
    marker = GATE_PREFIX.encode("ascii")
    checked = 0
    for target in sorted(targets.values(), key=lambda t: t["name"]):
        if target["type"] != "EXECUTABLE":
            continue
        models = {m for member in consistency.closure(target, targets)
                  for m in consistency.gekko_groups(member, include)}
        if not models or all(arch in ("default", "SSE2") for arch, _, _ in models):
            continue
        artifact = next((build / a["path"] for a in target.get("artifacts", [])
                         if a["path"].lower().endswith(".exe")), None)
        if artifact is None or not artifact.exists():
            continue
        checked += 1
        if marker not in artifact.read_bytes():
            failures.append(f"{target['name']}: compiled for {sorted(models)} but {artifact.name} carries no host ISA gate")
    if not failures:
        print(f"PASS E {checked} built executable(s) with a non-default host ISA model all carry the gate")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", required=True, type=pathlib.Path)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--dumpbin", required=True)
    parser.add_argument("--cmake")
    parser.add_argument("--arch", required=True, help="SHADOW_X64_ARCH of the build")
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--image", nargs=3, action="append", default=[], metavar=("NAME", "EXE", "MAP"),
                        help="image that must exist and be checked")
    parser.add_argument("--optional-image", nargs=3, action="append", default=[], metavar=("NAME", "EXE", "MAP"),
                        help="image checked only when it has been built")
    parser.add_argument("--contract-library", action="append", default=[])
    args = parser.parse_args()
    contract = {c.lower() for c in (args.contract_library or DEFAULT_CONTRACT_LIBRARIES)}
    failures = []
    images = [(n, pathlib.Path(e), pathlib.Path(m), True) for n, e, m in args.image] + \
             [(n, pathlib.Path(e), pathlib.Path(m), False) for n, e, m in args.optional_image]
    if not images:
        failures.append("no image to check")
    for name, exe, mapfile, required in images:
        if not exe.exists() or not mapfile.exists():
            if required:
                failures.append(f"{name}: {exe} or {mapfile} is missing (build it first)")
            else:
                print(f"SKIP {name}: not built")
            continue
        try:
            image, lmap = Image(exe), LinkerMap(mapfile)
            failures += lane_a(name, image, lmap)
            failures += lane_b(name, exe, lmap, args.dumpbin)
            fd, refused = lane_d(name, exe, args.work / name, args.arch)
            failures += fd
            failures += lane_c(name, exe, lmap, args.dumpbin, refused, contract)
        except Failure as exc:
            failures.append(str(exc))
    failures += lane_e(args.build.resolve(), args.source, args.config, args.cmake)
    if failures:
        print(f"FAIL host ISA gate: {len(failures)} problem(s)")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("PASS host ISA gate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
