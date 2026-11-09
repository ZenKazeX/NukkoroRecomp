"""Minimal reader for MSVC static libraries (.lib = ar archive of i386 COFF objects).

Gives, per object member: functions (name, section, offset, size) and the ordered calls each function makes
(REL32 relocations inside its extent), which is what the RenderWare aligner needs from the RW 3.7 PC libraries.
"""
from __future__ import annotations

import collections
import re
import struct
from dataclasses import dataclass, field

IMAGE_REL_I386_DIR32, IMAGE_REL_I386_REL32 = 0x06, 0x14
LOCAL_STATIC = re.compile(r"^\?(\w+)@\?\d+\?\?(\w+)@@9$")


@dataclass
class CoffFunction:
    name: str
    module: str
    section: int
    offset: int
    size: int
    calls: list[str] = field(default_factory=list)      # callee symbol names, in code order
    refs: list[str] = field(default_factory=list)       # DIR32 references (data / function pointers), in order
    strings: list[str] = field(default_factory=list)    # C strings it references (static data), in order
    code: bytes = b""                                   # the function's bytes (relocation fields unpatched)


def clean(sym: str) -> str:
    """C symbol as written in source: drop the leading underscore and a __stdcall @N suffix."""
    if sym.startswith("_"):
        sym = sym[1:]
    at = sym.rfind("@")
    if at > 0 and sym[at + 1:].isdigit():
        sym = sym[:at]
    return sym


def readable(sym: str) -> str:
    """MSVC's name for a function-local static, ?name@?N??Function@@9, as Function__name; other names unchanged."""
    m = LOCAL_STATIC.match(sym)
    return f"{m[2]}__{m[1]}" if m else sym


def members(path: str):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"!<arch>\n":
        raise ValueError(f"{path}: not an ar archive")
    pos, longnames = 8, b""
    while pos + 60 <= len(data):
        head = data[pos:pos + 60]
        name = head[:16].decode("latin-1").strip()
        size = int(head[48:58].decode().strip())
        body = data[pos + 60:pos + 60 + size]
        if name == "//":
            longnames = body
        elif name not in ("/", ""):
            if name.startswith("/") and name[1:].isdigit():
                off = int(name[1:])
                end = longnames.find(b"\0", off)
                end = longnames.find(b"/\n", off) if end < 0 else end
                name = longnames[off:end].decode("latin-1").rstrip("/")
            yield name.rstrip("/"), body
        pos += 60 + size + (size & 1)


def _read(body: bytes):
    """(sections, symbols) of one COFF object, or None when it is no i386 object. sections: (rawsize, relptr, nrel,
    characteristics, rawptr) per section; symbols: index -> (name, value, section number, type, storage class)."""
    if len(body) < 20:
        return None
    machine, nsec, _ts, symptr, nsym, optsize, _ch = struct.unpack_from("<HHIIIHH", body, 0)
    if machine != 0x14C or symptr == 0:
        return None
    strtab = symptr + nsym * 18

    def sname(raw: bytes) -> str:
        if raw[:4] == b"\0\0\0\0":
            off = struct.unpack_from("<I", raw, 4)[0]
            end = body.index(b"\0", strtab + off)
            return body[strtab + off:end].decode("latin-1")
        return raw.rstrip(b"\0").decode("latin-1")

    secs = []
    for i in range(nsec):
        o = 20 + optsize + i * 40
        _n, _vs, _va, rawsize, _rawptr, relptr, _ln, nrel, _nln, chars = struct.unpack_from("<8sIIIIIIHHI", body, o)
        secs.append((rawsize, relptr, nrel, chars, _rawptr))
    syms = {}
    i = 0
    while i < nsym:
        raw = body[symptr + i * 18:symptr + i * 18 + 18]
        value, secno, typ, cls, aux = struct.unpack_from("<IhHBB", raw, 8)
        syms[i] = (sname(raw[:8]), value, secno, typ, cls)
        i += 1 + aux
    return secs, syms


def parse_object(module: str, body: bytes) -> list[CoffFunction]:
    head = _read(body)
    if head is None:
        return []
    secs, syms = head
    funcs = []
    by_sec: dict[int, list] = {}
    for idx, (n, value, secno, typ, cls) in syms.items():
        if secno > 0 and (typ & 0x20) and cls in (2, 3):
            by_sec.setdefault(secno, []).append((value, n))
    for secno, lst in by_sec.items():
        lst.sort()
        rawsize, relptr, nrel, chars, _rp = secs[secno - 1]
        rels = []
        for k in range(nrel):
            va, si, rt = struct.unpack_from("<IIH", body, relptr + k * 10)
            rels.append((va, si, rt))
        rels.sort()
        for j, (value, n) in enumerate(lst):
            end = lst[j + 1][0] if j + 1 < len(lst) else rawsize
            fn = CoffFunction(clean(n), module, secno, value, end - value)
            fn.code = body[_rp + value:_rp + end] if _rp else b""
            for va, si, rt in rels:
                if value <= va < end and si in syms:
                    tgt = clean(syms[si][0])
                    if rt == IMAGE_REL_I386_REL32:
                        fn.calls.append(tgt)
                    elif rt == IMAGE_REL_I386_DIR32:
                        fn.refs.append(tgt)
                        _n2, tval, tsec, _t, _c = syms[si]
                        if tsec > 0 and not (secs[tsec - 1][3] & 0x20):       # not code: a data section
                            # (this probe once reused `end`, the function's extent, and so dropped every later
                            # relocation of the function: PC call lists were truncated until 2026-11-01)
                            txt = _cstring(body, secs[tsec - 1][4] + tval) if secs[tsec - 1][4] else None
                            if txt:
                                fn.strings.append(txt)
            funcs.append(fn)
    return funcs


def library_functions(path: str) -> list[CoffFunction]:
    out = []
    for module, body in members(path):
        out.extend(parse_object(module.split("\\")[-1], body))
    return out


@dataclass
class CoffData:
    name: str
    module: str
    size: int
    static: bool
    entries: dict = field(default_factory=dict)   # offset -> ("fn", name) | ("str", text) | ("data", name, addend)


def _cstring(body: bytes, start: int) -> str | None:
    end = body.find(b"\0", start, start + 256)
    if end <= start:
        return None
    try:
        txt = body[start:end].decode("ascii")
    except UnicodeDecodeError:
        return None
    return txt if txt.isprintable() and len(txt) >= 4 else None


def parse_data(module: str, body: bytes) -> list[CoffData]:
    """Named objects of the initialized data sections with the pointers they hold (DIR32 relocations), by offset.
    A pointer resolves to a function (code section), a C string, or another data object plus addend."""
    head = _read(body)
    if head is None:
        return []
    secs, syms = head
    code_at = {}                                       # (code section, offset) -> function name
    named = collections.defaultdict(list)              # data section -> [(offset, symbol index)]
    for idx, (n, value, secno, typ, cls) in syms.items():
        if secno <= 0 or cls not in (2, 3):
            continue
        if secs[secno - 1][3] & 0x20:
            if typ & 0x20:
                code_at[(secno, value)] = clean(n)
        elif secs[secno - 1][3] & 0x40 and not n.startswith((".", "$", "??_C@")):
            named[secno].append((value, idx))
    out = []
    for secno, lst in named.items():
        lst.sort()
        rawsize, relptr, nrel, chars, rawptr = secs[secno - 1]
        rels = sorted(struct.unpack_from("<IIH", body, relptr + k * 10) for k in range(nrel))
        for j, (value, idx) in enumerate(lst):
            end = lst[j + 1][0] if j + 1 < len(lst) else rawsize
            n, _v, _s, _t, cls = syms[idx]
            obj = CoffData(clean(n), module, end - value, cls == 3)
            for va, si, rt in rels:
                if not value <= va < end or rt != IMAGE_REL_I386_DIR32 or si not in syms:
                    continue
                tn, tval, tsec, ttyp, _c = syms[si]
                addend = struct.unpack_from("<I", body, rawptr + va)[0] if rawptr else 0
                if tsec <= 0:
                    obj.entries[va - value] = ("data", clean(tn), addend) if not ttyp & 0x20 else ("fn", clean(tn))
                elif secs[tsec - 1][3] & 0x20:
                    fn = clean(tn) if ttyp & 0x20 else code_at.get((tsec, tval + addend))
                    if fn:
                        obj.entries[va - value] = ("fn", fn)
                else:
                    txt = _cstring(body, secs[tsec - 1][4] + tval + addend) if secs[tsec - 1][4] else None
                    obj.entries[va - value] = ("str", txt) if txt else ("data", clean(tn), addend)
            out.append(obj)
    return out


def library_data(path: str) -> list[CoffData]:
    out = []
    for module, body in members(path):
        out.extend(parse_data(module.split("\\")[-1], body))
    return out
