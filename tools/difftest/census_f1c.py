"""F1c machine-code census of the generated transfer sites in a linked F1c driver (test-only).

Reads the driver's linker map (/MAP) for symbol addresses and disassembles every generated fn_* code
symbol of the F1c fixture with capstone. Every CALL and JMP whose target is a translated transfer
(a fn_* symbol - public entry, entry thunk, _at, _body, _body_at - an out-of-line recomp:: transfer
helper, or an indirect register/memory target, which inside generated code is the `native(c)` of an
inlined call_indirect / jump_indirect / resume_elsewhere) is classified:

  call_ret   CALL whose fall-through reaches `ret` executing only epilogue instructions (restore of
             non-volatiles from [rsp+x], add rsp, pop, nop, unconditional jmp to such a block): the
             caller's frame stays on the host stack for the whole callee, although the guest transfer
             has nothing left to do. A guest cycle through such a site grows the host stack.
  call_cont  CALL followed by real work (a guest call that returns into the caller: expected).
  tail_jmp   JMP: stack-neutral.

usage: census_f1c.py --exe f1c_stack_driver.exe --map f1c_stack_driver.map --fixture f1c_fixture.json
                     --out census.json
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from pathlib import Path

SYM = re.compile(r"^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s+f\s+(?:i\s+)?(\S+)\s*$")
EPILOGUE = re.compile(r"^(mov r[a-z0-9]+, qword ptr \[rsp \+ 0x[0-9a-f]+\]|movaps xmm[0-9]+, xmmword ptr \[rsp"
                      r"(?: \+ 0x[0-9a-f]+)?\]|add rsp, 0x[0-9a-f]+|lea rsp, \[r[a-z0-9]+(?: [+-] 0x[0-9a-f]+)?\]"
                      r"|pop r[a-z0-9]+|nop.*|int3)$")
FN = re.compile(r"^\?(fn_([0-9A-F]{8})(?:_e([0-9A-F]{8})|_at|_body|_body_at)?)@recomp@gekko@@")
HELPERS = ("jump_indirect", "resume_elsewhere", "call_indirect")


def demangle_plain(name: str) -> str:
    return name[1:].split("@", 1)[0] if name.startswith("?") else name


def census(exe: Path, map_path: Path, fixture: dict) -> dict:
    import capstone
    import pefile

    symbols = {}
    generated = []
    for line in map_path.read_text(encoding="latin-1").splitlines():
        m = SYM.match(line)
        if not m:
            continue
        name, address, obj = m.group(1), int(m.group(2), 16), m.group(3)
        symbols.setdefault(address, name)
        if re.match(r"recomp_\d{3}\.obj$", obj) and FN.match(name):
            generated.append((address, name))
    starts = sorted(symbols)
    helper = {a: demangle_plain(n) for a, n in symbols.items()
              if demangle_plain(n) in HELPERS and "@recomp@gekko@@" in n}
    fn_kind = {}
    for address, name in generated:
        plain = FN.match(name).group(1)
        if plain.endswith("_body") or plain.endswith("_body_at"):
            fn_kind[address] = "body"
        elif re.search(r"_e[0-9A-F]{8}$", plain):
            fn_kind[address] = "entry_thunk"
        elif plain.endswith("_at"):
            fn_kind[address] = "at_thunk"
        else:
            fn_kind[address] = "public"

    pe = pefile.PE(str(exe), fast_load=True)
    text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
    base = pe.OPTIONAL_HEADER.ImageBase + text.VirtualAddress
    data = text.get_data()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    def returns_through_epilogue(address: int, depth: int = 0) -> bool:
        if depth > 6:
            return False
        offset = address - base
        for count, insn in enumerate(md.disasm(data[offset:offset + 96], address)):
            text_i = insn.mnemonic + (" " + insn.op_str if insn.op_str else "")
            if insn.mnemonic == "ret":
                return True
            if insn.mnemonic == "jmp" and insn.op_str.startswith("0x"):
                return returns_through_epilogue(int(insn.op_str, 16), depth + 1)
            if not EPILOGUE.match(text_i) or count > 16:
                return False
        return False

    # guest row address -> shape name
    owner = {}
    for shape in fixture.get("shapes", []):
        for row in shape["rows"]:
            owner[row] = shape["name"]
    for program in fixture.get("canary", []):
        for row in program.get("rows", []):
            owner[row] = "canary:" + program["name"]

    per_shape = {}
    totals = {}
    for address, name in generated:
        m = FN.match(name)
        row = int(m.group(2), 16)
        shape = owner.get(row, "support")
        index = bisect.bisect_right(starts, address)
        end = starts[index] if index < len(starts) else base + len(data)
        offset = address - base
        entry = per_shape.setdefault(shape, {"call_ret": [], "counts": {}})
        for insn in md.disasm(data[offset:end - base], address):
            if insn.mnemonic not in ("call", "jmp"):
                continue
            op = insn.op_str
            if op.startswith("0x"):
                target = int(op, 16)
                if target in helper:
                    kind = helper[target]
                elif target in fn_kind:
                    kind = "fn_" + fn_kind[target]
                else:
                    continue
                target_name = demangle_plain(symbols.get(target, hex(target)))
            elif insn.mnemonic == "call" or re.match(r"^r[a-z0-9]+$", op) or op.startswith("qword ptr"):
                if insn.mnemonic == "jmp" and not (re.match(r"^r[a-z0-9]+$", op) or op.startswith("qword ptr")):
                    continue
                kind = "indirect"
                target_name = op
            else:
                continue
            if insn.mnemonic == "jmp":
                cls = "tail_jmp"
            else:
                cls = "call_ret" if returns_through_epilogue(insn.address + insn.size) else "call_cont"
            key = f"{kind}/{cls}"
            entry["counts"][key] = entry["counts"].get(key, 0) + 1
            totals[key] = totals.get(key, 0) + 1
            if cls == "call_ret":
                entry["call_ret"].append({"site": f"0x{insn.address:x}", "in": demangle_plain(name),
                                          "target": target_name, "kind": kind})
    return {"schema": "gekko-f1c-census/1", "exe": str(exe), "totals": dict(sorted(totals.items())),
            "shapes": dict(sorted(per_shape.items()))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    fixture = json.loads(args.fixture.read_text(encoding="utf-8"))
    result = census(args.exe, args.map, fixture)
    args.out.write_text(json.dumps(result, indent=1), encoding="utf-8")
    for shape, info in result["shapes"].items():
        kinds = sorted({c["kind"] for c in info["call_ret"]})
        print(f"{shape:24} call_ret={len(info['call_ret']):3} {','.join(kinds)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
