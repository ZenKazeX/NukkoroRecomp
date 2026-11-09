"""Outline of one translated function (.build/watch/generated): its direct calls, indirect calls, SDA (r13 / r2)
accesses, absolute address constants and memory offsets from each base register - enough to see what a function
does before reading it. Reading aid only; the generated code is the authority.
Usage: python -B tools/dolrecon/fn_outline.py <address hex> [generated dir] [--body]
"""
import glob
import re
import sys

addr = sys.argv[1].upper().replace("0X", "")
gen = next((a for a in sys.argv[2:] if not a.startswith("--")), ".build/watch/generated")
body_wanted = "--body" in sys.argv
name = f"fn_{addr}_body"
for path in glob.glob(gen + "/recomp_*.cpp"):
    text = open(path, encoding="utf-8", errors="replace").read()
    i = text.find(f"static void {name}(Cpu& c, u32 entry) {{")
    if i < 0:
        continue
    j = text.find("\nstatic void ", i + 10)
    body = text[i: j if j > 0 else len(text)]
    calls = re.findall(r"\b(fn_[0-9A-F]{8})\(c\)", body)
    indirect = len(re.findall(r"call_indirect", body))
    sda = sorted(set(re.findall(r"c\.gpr\[(13|2)\] \+ (0x[0-9A-F]+)u", body)))
    consts = sorted(set(re.findall(r"0x(80[0-9A-F]{6})u", body)) - {addr})
    loads = re.findall(r"ea = c\.gpr\[(\d+)\] \+ (0x[0-9A-F]+)u;\s*\n\s*(c\.(?:gpr|fpr)\[\d+\] = c\.mem_read(\d+)|c\.mem_write(\d+))", body)
    print(f"{name} in {path}: {body.count(chr(10))} lines")
    print("  calls   :", " ".join(dict.fromkeys(calls)) or "-")
    print("  indirect:", indirect)
    print("  SDA     :", ", ".join(f"r{b}+{o}" for b, o in sda) or "-")
    print("  consts  :", " ".join(c for c in consts if not c.startswith(addr[:4]))[:400] or "-")
    acc = {}
    for base, off, what, rd, wr in loads:
        acc.setdefault(f"r{base}", set()).add(("R" if rd else "W") + (rd or wr) + "@" + off)
    for b, s in sorted(acc.items()):
        if b != "r1":
            print(f"  {b:4s}    :", " ".join(sorted(s)))
    if body_wanted:
        print(body)
    break
else:
    sys.exit(f"{name} not found under {gen}")
