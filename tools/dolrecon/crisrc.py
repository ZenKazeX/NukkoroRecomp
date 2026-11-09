"""Index of CRI's ADX library source (reference only): functions in source order, their strings and callees.

Source: the public matching decompilation crowded-street/3s-decomp (Street Fighter III 3rd Strike, PS2), checked
out read-only at H:/Tools/refs/3s-decomp; src/anniversary/cri/libadxe/*.c is CRI's libadxe (ADXT 8.34, ADXF 6.33,
CVFS 2.34; Shadow carries ADXT/GC 9.44, ADXF/GC 7.30, CVFS/GC 2.39). Functions not yet decompiled there are
INCLUDE_ASM stubs: their name and position are known, their body is not.

index(root) -> [SrcFunction] in file order; every C string literal of a body (adjacent literals joined, escapes
decoded) and every call to another indexed function, in source order.
"""
from __future__ import annotations

import os
import re
from dataclasses import dataclass, field

ROOT = r"H:\Tools\refs\3s-decomp\src\anniversary\cri\libadxe"
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else", "do", "case", "goto"}
STUB = re.compile(r'INCLUDE_ASM\(\s*"[^"]*"\s*,\s*(\w+)\s*\)')
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
ESC = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'"}


@dataclass
class SrcFunction:
    name: str
    file: str
    order: int
    body: str | None
    strings: list[str] = field(default_factory=list)
    calls: list[str] = field(default_factory=list)


def _strip(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _unescape(s: str) -> str:
    return re.sub(r"\\(.)", lambda m: ESC.get(m.group(1), m.group(1)), s)


def _strings(body: str) -> list[str]:
    out, cur, last_end = [], None, None
    for m in LITERAL.finditer(body):
        if cur is not None and body[last_end:m.start()].strip() == "":
            cur += _unescape(m.group(1))                 # "abc" "def" -> one literal
        else:
            if cur is not None:
                out.append(cur)
            cur = _unescape(m.group(1))
        last_end = m.end()
    if cur is not None:
        out.append(cur)
    return out


def index(root: str = ROOT) -> list[SrcFunction]:
    funcs = []
    for fname in sorted(os.listdir(root)):
        if not fname.endswith(".c"):
            continue
        with open(os.path.join(root, fname), encoding="utf-8", errors="replace") as f:
            text = _strip(f.read())
        order, depth, i, last = 0, 0, 0, 0
        n = len(text)
        while i < n:
            ch = text[i]
            if ch == '"':                                    # skip literals when tracking braces
                j = i + 1
                while j < n and text[j] != '"':
                    j += 2 if text[j] == "\\" else 1
                i = j + 1
                continue
            if depth == 0:
                m = STUB.match(text, i)
                if m:
                    funcs.append(SrcFunction(m.group(1), fname, order, None))
                    order += 1
                    i = last = m.end()
                    continue
            if ch == "{":
                if depth == 0:
                    head = text[last:i]
                    h = re.search(r"\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*$", head)
                    if h and h.group(1) not in KEYWORDS and "=" not in head.split("(")[0]:
                        j, d = i, 0
                        while j < n:
                            if text[j] == '"':
                                k = j + 1
                                while k < n and text[k] != '"':
                                    k += 2 if text[k] == "\\" else 1
                                j = k + 1
                                continue
                            d += text[j] == "{"
                            d -= text[j] == "}"
                            if d == 0:
                                break
                            j += 1
                        funcs.append(SrcFunction(h.group(1), fname, order, text[i:j + 1]))
                        order += 1
                        i = last = j + 1
                        continue
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    last = i + 1
            elif ch == ";" and depth == 0:
                last = i + 1
            i += 1
    names = {f.name for f in funcs}
    for f in funcs:
        if f.body:
            f.strings = _strings(f.body)
            f.calls = [m.group(1) for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", LITERAL.sub('""', f.body))
                       if m.group(1) in names and m.group(1) != f.name or m.group(1) == f.name and False]
    return funcs
