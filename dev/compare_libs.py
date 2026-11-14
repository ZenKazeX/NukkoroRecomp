"""Compares two builds' static libraries object by object (the prebuilt parts against a player's own build of them).

    python dev/compare_libs.py <dir A> <dir B> [name.lib ...]

Each .lib (an ar archive of COFF objects) is split into its members; members are paired by file name (the folders the
two trees used differ) and compared byte for byte after zeroing the one field a compile always changes, the COFF
header's TimeDateStamp. A member that still differs is listed with the first differing offset and the bytes there.
"""
import re
import struct
import sys

# --anonymous: also treat MSVC's anonymous-namespace names (?A0x<hash>; the hash comes from the source file's path) as equal.
ANONYMOUS = '--anonymous' in sys.argv
if ANONYMOUS:
    sys.argv.remove('--anonymous')
from pathlib import Path


def members(path: Path):
    data = path.read_bytes()
    if data[:8] != b'!<arch>\n':
        raise SystemExit(f'{path}: not an archive')
    pos, longnames, out = 8, b'', {}
    while pos + 60 <= len(data):
        header = data[pos:pos + 60]
        name = header[:16].decode('latin-1').rstrip()
        size = int(header[48:58].decode().strip())
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name == '//':
            longnames = body
            continue
        if name == '/' or name == '/<ECSYMBOLS>/':
            continue
        if name.startswith('/') and name[1:].isdigit():
            start = int(name[1:])
            end = longnames.find(b'\0', start)
            name = longnames[start:end].decode('latin-1')
        name = name.rstrip('/').replace('\\', '/').split('/')[-1].lower()
        n = 2
        base = name
        while name in out:
            name = f'{base}#{n}'
            n += 1
        out[name] = body
    return out


def normalise(obj: bytes) -> bytes:
    """What of an object can reach a program the linker makes without /DEBUG: every section but .debug$* (contents,
    flags, relocations) and the symbol table (names, values, sections, types). Left out: the header's TimeDateStamp,
    file offsets, and the .debug$* sections (CodeView: they hold the object's own path, S_OBJNAME, and the linker
    drops them when it writes no PDB)."""
    if obj[0:4] == b'\x00\x00\xff\xff':  # /bigobj: ANON_OBJECT_HEADER_BIGOBJ
        nsec, symptr, nsym = struct.unpack_from('<III', obj, 44)
        sec_off, sym_size, sec_field = 56, 20, '<i'
    else:
        _, nsec, _, symptr, nsym, optsize, _ = struct.unpack_from('<HHIIIHH', obj, 0)
        sec_off, sym_size, sec_field = 20 + optsize, 18, '<h'
    strtab = obj[symptr + nsym * sym_size:]
    out = bytearray()

    def name_of(raw8: bytes) -> bytes:
        if raw8[:1] == b'/' and raw8[1:].rstrip(b'\0').isdigit():
            start = int(raw8[1:].rstrip(b'\0'))
            return strtab[start:strtab.index(b'\0', start)]
        return raw8.rstrip(b'\0')

    for i in range(nsec):
        s = sec_off + 40 * i
        name = name_of(obj[s:s + 8])
        size, rawptr, relptr, _, nrel, _, flags = struct.unpack_from('<IIIIHHI', obj, s + 16)
        out += b'[%d %s %08x]' % (i, name, flags)
        if name.startswith(b'.debug$') or name == b'.chks64':  # .chks64: checksums of the sections, debug ones too
            continue
        out += obj[rawptr:rawptr + size] if rawptr else b'(bss %d)' % size
        out += obj[relptr:relptr + 10 * nrel]
    i = 0
    while i < nsym:
        e = obj[symptr + i * sym_size: symptr + (i + 1) * sym_size]
        if e[:4] == b'\0\0\0\0':
            off = struct.unpack_from('<I', e, 4)[0]
            sym = strtab[off:strtab.index(b'\0', off)]
        else:
            sym = e[:8].rstrip(b'\0')
        aux = e[sym_size - 1]
        out += b'{%s}' % sym + e[8:]
        if not (sym.startswith(b'.debug$') or sym == b'.chks64'):  # a debug section's aux record holds its length and checksum
            out += obj[symptr + (i + 1) * sym_size: symptr + (i + 1 + aux) * sym_size]
        i += 1 + aux
    if ANONYMOUS:
        out = re.sub(rb'\?A0x[0-9a-f]{8}', b'?A0x________', bytes(out))
    return bytes(out)


def compare(a: Path, b: Path):
    ma, mb = members(a), members(b)
    same = differ = 0
    notes = []
    for name in sorted(set(ma) | set(mb)):
        if name not in ma or name not in mb:
            notes.append(f'  only in {"A" if name in ma else "B"}: {name}')
            differ += 1
            continue
        x, y = normalise(ma[name]), normalise(mb[name])
        if x == y:
            same += 1
            continue
        differ += 1
        first = next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))
        notes.append(f'  differs: {name} ({len(x)} vs {len(y)} bytes), first at {first:#x}: '
                     f'{x[first:first + 24]!r} vs {y[first:first + 24]!r}')
    return same, differ, notes


def main():
    a, b = Path(sys.argv[1]), Path(sys.argv[2])
    names = sys.argv[3:] or sorted(p.name for p in a.glob('*.lib'))
    bad = 0
    for name in names:
        same, differ, notes = compare(a / name, b / name)
        print(f'{name}: {same} objects identical, {differ} differ')
        for line in notes:
            print(line)
        bad += differ
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
