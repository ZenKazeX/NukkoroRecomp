"""How the native runtime depends on GameCube SDK internals: every frontier row of the removal plan that the
runtime's own sources enter, with each use classified by the runtime API it goes through.

    python -B runtime_coupling.py [--plan <removal_plan.json>] [--out <dir>]

A frontier row (tools/platform/removal_plan.py) is an SDK row entered from outside the SDK. This tool takes the
entries that are runtime literals and follows each literal in the shipping runtime sources (runtime/src,
runtime/include, runtime/lift, runtime/backends; tests are listed separately) to its uses: the line naming it and,
for `constexpr ... NAME = 0x...` constants, every line using NAME in the files that can see it. Each use is labelled
by the API it goes through:

  owner-hook     install_hle / install_chained_hle on the address (native code serves or wraps that entry)
  owner-table    a {0xADDR, ...} row of an array whose element type is Address / *OwnerEntry / *StopEntry (the
                 array's declaration line says so; the file installs its rows as owners)
  owner-call     serve_owner(ADDR): native code enters another native owner (never the guest body)
  observer       observe* / observer: native code watches the entry, the original body still runs
  guest-call     call_guest* / guest_call / a local call(kNAME or 0xADDR): native code calls the SDK function (a native -> SDK
                 dependency). Only frontier (platform) rows are looked at, so a guest_call of RW core is not counted
  lowered-site   a site table row {0xADDR, 0xWORD, "kind"}: an instruction INSIDE the SDK body that native code
                 serves (the SDK body itself still runs around it)
  probe          a measurement probe table row (diagnostics)
  comparison     the address compared with a PC / LR / target
  declaration    the constant's own definition (not a use)
  unclassified   none of the above: read the line (listed with file:line)

Nothing is inferred beyond the line's text; an unclassified use is a work item, not a guess.
Writes <out>/runtime_coupling.json and <out>/RUNTIME_COUPLING.md.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", ".."))
SHIPPING = ("runtime/src/", "runtime/include/", "runtime/lift/", "runtime/backends/")
SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".cc", ".inc")

RULES = [
    ("owner-hook", re.compile(r"\binstall(_chained)?_hle\s*\(|\binstall_after_existing\s*\(|(?<![\w.>])own\s*\(\s*k[A-Z]")),
    # a row of a local {address, "name"} table a loop installs as owners
    ("owner-table", re.compile(r"\b[A-Z]\w*\{\s*k[A-Z]\w*\s*,\s*\"")),
    # native code enters another native owner by address (Cpu::serve_owner runs only an installed owner, never the
    # guest body; it returns false when none is installed)
    ("owner-call", re.compile(r"\bserve_owner\s*\(")),
    # native code transfers control into the SDK body (the guest OS scheduler, a yield): a native -> SDK dependency
    ("dispatch-to-sdk", re.compile(r"\bleave_to_dispatcher\s*\(")),
    ("guest-call", re.compile(r"\bcall_guest\w*\s*\(|\bguest_call\s*\(|(?<![\w.>])call\s*\(\s*(?:c\s*,\s*)?(?:k[A-Z]\w*|0x[0-9A-Fa-f]{8})")),
    ("observer", re.compile(r"\bobserve\w*\s*\(|\bobserver\b|^\s*\{\s*0x[0-9A-Fa-f]{8}u?\s*,\s*\"[a-z][a-z_-]*\"\s*\}")),
    ("lowered-site", re.compile(r"^\s*\{\s*0x[0-9A-Fa-f]{8}u?\s*,\s*0x[0-9A-Fa-f]{8}u?\s*,\s*\"[a-z_0-9]+\"")),
    ("probe", re.compile(r"^\s*\{\s*\"r\d+\"\s*,\s*\"[a-z_0-9]+\"\s*,\s*0x[0-9A-Fa-f]{8}u?|\badd_probe\s*\(")),
    # a return address identifying which SDK caller produced something (no control transfer)
    ("lr-identity", re.compile(r"^\s*\{\s*0x[0-9A-Fa-f]{8}u?\s*,\s*native::DrawProducer::|\blr\s*[<>]=?|kRasterFlushLr|\breport_rw_error\s*\(")),
    ("trace-or-message", re.compile(r"\bnote_movie_event\s*\(|\bcase\s+0x[0-9A-Fa-f]{8}u?\s*:\s*name\s*=|"
                                    r"\b(snprintf|appendf|fprintf|printf)\s*\(|<<\s*\"|^\s*0x[0-9A-Fa-f]{8}u?\s*,\s*$")),
    # a word of a constant data image a native owner writes into guest memory (offset, value): the guest reads it later
    # (e.g. the GX library's region callbacks, which guest code calls through); no control transfer by native code
    ("data-word", re.compile(r"\{\s*0x[0-9A-Fa-f]{3}\s*,\s*0x8[0-9A-Fa-f]{7}u?\s*\}")),
    ("address-value", re.compile(r"^c\.gpr\[\d+\]\s*=\s*k[A-Z]\w*\s*;|\bmem_write32\([^;]*,\s*k[A-Z]\w*\s*\)\s*;")),
    ("address-arith", re.compile(r"\bk[A-Z]\w*\s*\+\s*\d")),
    ("comparison", re.compile(r"(==|!=)\s*[A-Za-z_]*\b|\b(pc|lr|target|address|site)\b\s*(==|!=)")),
]
DECLARATOR = re.compile(r"\b(k[A-Z]\w*)\s*=\s*0x([0-9A-Fa-f]{8})u?\b")
CONST = re.compile(r"\b(?:static\s+)?(?:inline\s+)?constexpr\s+(?:gekko::)?(?:u32|std::uint32_t|uint32_t)\s+"
                   r"([A-Za-z_]\w*)\s*=\s*0x([0-9A-Fa-f]{8})u?\b")


def declarators(lines, n, code):
    """{name: value} of the constants line n declares: every `kName = 0x8...` of a constexpr statement."""
    if "constexpr" not in statement(lines, n):
        return {}
    return {m.group(1): int(m.group(2), 16) for m in DECLARATOR.finditer(code)}


def lifted_uncompiled():
    """The sources the lifted (shipping) build leaves out: CMakeLists.txt's SHADOW_LIFTED_UNCOMPILED_SOURCES."""
    text = open(os.path.join(SRC, "CMakeLists.txt"), encoding="utf-8").read()
    block = re.search(r"set\(SHADOW_LIFTED_UNCOMPILED_SOURCES(.*?)\)\s*$", text, flags=re.S | re.M)
    return set(re.findall(r"^\s*(runtime/\S+?)\)?\s*$", block.group(1), flags=re.M)) if block else set()


def native_owner_rows():
    rows = set()
    for name in ("native-owners.txt", "native-owners-r2.txt"):
        for line in open(os.path.join(SRC, "extract", name), encoding="utf-8"):
            if line.startswith("0x"):
                rows.add(int(line.split()[0], 16))
    return rows


def refine(use, statement_text, name, value, owned):
    """Facts of the statement that the line-level rules cannot see."""
    if use == "guest-call" and name:
        target = re.search(r"\bguest_call\s*\(\s*\w+\s*,\s*(\w+)", statement_text)
        if target and target.group(1) != name:
            return "lr-identity"        # the constant is the call's link argument, not its target
    if use == "dispatch-to-sdk" and value in owned:
        return "owner-dispatch"         # re-enters a native owner's row through the dispatcher, no SDK body runs
    return use


def strip_comment(line):
    out, i, q = [], 0, None
    while i < len(line):
        c = line[i]
        if q:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == q:
                q = None
        elif c in "\"'":
            q = c
            out.append(c)
        elif line.startswith("//", i):
            break
        else:
            out.append(c)
        i += 1
    return "".join(out)


TABLE_DECL = re.compile(r"\b(Address|\w*OwnerEntry|\w*StopEntry)\s+\w+\s*\[\s*\]\s*=\s*\{")
TABLE_ROW = re.compile(r"^\s*\{\s*(?:0x[0-9A-Fa-f]{8}u?|k[A-Z]\w*)\s*,")


def owner_table_rows(lines):
    """Line numbers (1-based) of rows inside an array declared with an *OwnerEntry / *StopEntry element type."""
    rows, inside = set(), False
    for n, line in enumerate(lines, 1):
        code = strip_comment(line)
        if TABLE_DECL.search(code):
            inside = True
            continue
        if inside:
            if code.strip().startswith("};"):
                inside = False
            elif TABLE_ROW.search(code):
                rows.add(n)
    return rows


RANGE_FOR = re.compile(r"\bfor\s*\([^:;]*:\s*\{[^}]*\}\s*\)\s*\{\s*$")


def owner_loop(lines, n):
    """Line n (1-based) is a range-for over a braced constant list whose body starts with install_hle: each
    constant is installed as an owner (e.g. `for (const u32 a : {kA, kB}) { cpu.install_hle(a, ...`)."""
    if not RANGE_FOR.search(strip_comment(lines[n - 1])):
        return False
    for line in lines[n:n + 3]:
        code = strip_comment(line).strip()
        if code:
            return bool(re.search(r"\binstall(_chained)?_hle\s*\(", code))
    return False


def classify(code):
    for name, rx in RULES:
        if rx.search(code):
            return name
    return "unclassified"


def statement(lines, n):
    """The statement holding line n (1-based): continuation lines joined back to where it starts."""
    i = n - 1
    parts = [strip_comment(lines[i]).strip()]
    while i > 0 and len(parts) < 8:
        prev = strip_comment(lines[i - 1]).strip()
        if not prev or prev.endswith((";", "{", "}")) or prev.startswith("#"):
            break
        parts.insert(0, prev)
        i -= 1
    return " ".join(parts)


def load_sources():
    files = {}
    for d in ("runtime",):
        for dp, _, fs in os.walk(os.path.join(SRC, d)):
            for f in fs:
                if f.endswith(SUFFIXES):
                    p = os.path.join(dp, f)
                    rel = os.path.relpath(p, SRC).replace(os.sep, "/")
                    if rel.startswith("runtime/tests/generated/"):
                        continue        # the lift: reference-interpreter-only, roots nothing
                    files[rel] = open(p, encoding="utf-8", errors="replace").read().splitlines()
    return files


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--plan", default=os.path.join(SRC, ".build", "platform", "removal_plan", "removal_plan.json"))
    ap.add_argument("--out", default=os.path.join(SRC, ".build", "platform", "runtime_coupling"))
    ap.add_argument("--require-classified", action="store_true",
                    help="fail when a shipping use matches no rule (a new kind of runtime use of SDK internals)")
    ap.add_argument("--max-sdk-calls", type=int, default=None,
                    help="ratchet: fail when native code calls / dispatches into SDK bodies at more sites than this")
    args = ap.parse_args(argv)
    plan = json.load(open(args.plan))
    files = load_sources()

    # literal -> [(file, line number, code)]; constant name -> literal (per file)
    lit_lines = collections.defaultdict(list)
    consts = collections.defaultdict(dict)     # file -> {NAME: literal}
    for rel, lines in files.items():
        for n, line in enumerate(lines, 1):
            code = strip_comment(line)
            for m in re.finditer(r"0[xX](8[0-9A-Fa-f]{7})(?![0-9A-Fa-f])", code):
                in_string = code[:m.start()].count('"') % 2 == 1          # inside a string literal: a message
                lit_lines[int(m.group(1), 16)].append((rel, n, code.strip(), in_string))
            for m in CONST.finditer(code):
                consts[rel][m.group(1)] = int(m.group(2), 16)
            consts[rel].update(declarators(lines, n, code))

    table_rows = {rel: owner_table_rows(lines) for rel, lines in files.items()}
    owned = native_owner_rows()
    not_shipped = lifted_uncompiled()

    def shipped(path):
        return path.startswith(SHIPPING) and path not in not_shipped

    def uses_of(literal):
        out = []
        for rel, n, code, in_string in lit_lines.get(literal, ()):
            if in_string:
                kind = "trace-or-message"
            else:
                kind = "owner-table" if n in table_rows[rel] else classify(statement(files[rel], n))
            declared = [k for k, v in declarators(files[rel], n, code).items() if v == literal]
            if declared:
                out.append({"file": rel, "line": n, "use": "declaration", "code": code[:200]})
                name = declared[0]
                # the constant's uses: this file, and (for a header) every file including it
                scope = [rel] + ([f for f, ls in files.items() if any(os.path.basename(rel) in l and "#include" in l
                                                                       for l in ls)] if rel.endswith(".h") else [])
                word = re.compile(r"\b" + re.escape(name) + r"\b")
                for f in scope:
                    for k, l in enumerate(files[f], 1):
                        c = strip_comment(l)
                        if word.search(c) and name not in declarators(files[f], k, c):
                            stmt = statement(files[f], k)
                            use = ("owner-table" if k in table_rows[f] else
                                   "owner-hook" if owner_loop(files[f], k) else refine(classify(stmt), stmt, name, literal, owned))
                            out.append({"file": f, "line": k, "use": use, "via": name,
                                        "code": c.strip()[:200]})
            else:
                out.append({"file": rel, "line": n, "use": kind, "code": code[:200]})
        return out

    rows = []
    for service, s in plan["services"].items():
        for row, why in s["frontier_rows"].items():
            lits = []
            for w in why:
                if w.startswith("runtime-literal: "):
                    lits.append(int(w.split()[1], 16))
            if not lits:
                continue
            other = sorted({w.split(":")[0] for w in why if not w.startswith("runtime-literal")})
            uses = [u for l in lits for u in uses_of(l)]
            ship = [u for u in uses if shipped(u["file"])]
            kinds = collections.Counter(u["use"] for u in ship if u["use"] != "declaration")
            rows.append({"service": service, "row": row, "literals": [f"0x{l:08X}" for l in lits],
                         "runtime_only": not other, "also_entered_by": other,
                         "shipping_uses": dict(kinds), "uses": uses})

    os.makedirs(args.out, exist_ok=True)
    json.dump(rows, open(os.path.join(args.out, "runtime_coupling.json"), "w"), indent=1)
    by_kind = collections.Counter()
    for r in rows:
        for k in r["shipping_uses"]:
            by_kind[k] += 1
    md = ["# Runtime-held SDK coupling (generated by tools/platform/runtime_coupling.py; do not edit)", "",
          f"Frontier rows the runtime's sources enter: {len(rows)}; entered by nothing else: "
          f"{sum(r['runtime_only'] for r in rows)}.", "",
          "| use | rows |", "|---|---:|"] + [f"| {k} | {v} |" for k, v in by_kind.most_common()]
    for r in sorted(rows, key=lambda r: (not r["runtime_only"], r["service"], r["row"])):
        md += ["", f"## `{r['row']}` ({r['service']}{', runtime only' if r['runtime_only'] else ', also ' + ', '.join(r['also_entered_by'])})", ""]
        for u in r["uses"]:
            tag = "" if shipped(u["file"]) else " (test)"
            md.append(f"- {u['use']}{tag}: `{u['file']}:{u['line']}` {('[' + u['via'] + '] ') if 'via' in u else ''}`{u['code'][:140]}`")
    open(os.path.join(args.out, "RUNTIME_COUPLING.md"), "w", encoding="utf-8", newline="\n").write("\n".join(md) + "\n")
    print(f"{len(rows)} frontier rows entered by runtime sources ({sum(r['runtime_only'] for r in rows)} only by them)")
    for k, v in by_kind.most_common():
        print(f"  {v:4}  {k}")
    unc = [(r["row"], u["file"], u["line"]) for r in rows for u in r["uses"]
           if u["use"] == "unclassified" and shipped(u["file"])]
    print(f"  unclassified shipping uses: {len(unc)}")
    for row, f, n in unc:
        print(f"    {row}: {f}:{n}")
    calls = sum(1 for r in rows for u in r["uses"]
                if u["use"] in ("guest-call", "dispatch-to-sdk") and shipped(u["file"]))
    if args.max_sdk_calls is not None and calls > args.max_sdk_calls:
        print(f"COUPLING: {calls} native -> SDK calls / dispatches, more than the ratchet's {args.max_sdk_calls}")
        return 1
    return 1 if args.require_classified and unc else 0


if __name__ == "__main__":
    sys.exit(main())
