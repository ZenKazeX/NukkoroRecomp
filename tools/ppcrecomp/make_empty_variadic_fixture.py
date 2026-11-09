"""Pin the empty-leaf contract and compile an isolated original-image fixture.

Checks encoded direct edges and aligned data references. This does not prove
arbitrary computed targets, stack lifetime in every caller, or gameplay parity.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from graphics_lift_inventory import Dol
from gx_site_inventory import Image as FlowImage, read_jump_tables
from lift.type_call_liveness import Tracer

DOL_SHA256 = 'fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af'
# entry, bytes, original body SHA-256, external direct call count, edge SHA-256
LEAVES = (
    (0x8000D374, 80, 'd0cae8c3f5437f967d82becb892c3c5344c4113435cef83642dc602d29052bbe',
     309, '6af6cc2aaf988c0277192cb3f4cfd231de305e293c7a91a3fe32ec6cd05de97e'),
    (0x8001C574, 84, '305560cc66a5ab34c4aced4002dd59d1cfe8de2371d89b8d8dbd667812793d03',
     332, '10fa7efb3a5ce91d3f64dcee6ffad3c9ff899d25e48217ba5edcabfda4ebbaed'),
    (0x8047EC78, 104, '68a72106bfd1cbc4af87bf955bcd450fc2c6f73e858fdac96ec431d614d41247',
     300, '7205f703df9c6862428cc3314139bd47a56eb030be2b5fc2c6340f6c3f963ebf'),
)


def audit(raw, listed):
    if hashlib.sha256(raw).hexdigest() != DOL_SHA256:
        raise ValueError('empty-leaf contract requires the verified original DOL')
    addresses = [int(line.split('#')[0].strip(), 16) for line in listed.splitlines()
                 if line.split('#')[0].strip()]
    if sorted(addresses) != [row[0] for row in LEAVES]:
        raise ValueError('empty-leaf list differs from the reviewed three entries')
    sections = []
    for i in range(18):
        offset, address, size = (struct.unpack_from('>I', raw, base + i*4)[0]
                                 for base in (0, 0x48, 0x90))
        if offset + size > len(raw):
            raise ValueError('DOL section extends outside file')
        if size:
            sections.append((i < 7, address, raw[offset:offset+size]))
    records = []
    for entry, size, digest, count, edge_digest in LEAVES:
        body = next(data[entry-base:entry-base+size] for text, base, data in sections
                    if text and base <= entry and entry+size <= base+len(data))
        if hashlib.sha256(body).hexdigest() != digest:
            raise ValueError('empty-leaf body changed')
        edges = []
        for text, base, data in sections:
            for off in range(0, len(data)-3, 4):
                site = base+off
                word, = struct.unpack_from('>I', data, off)
                if not text:
                    if entry <= word < entry+size:
                        raise ValueError(f'data reference to empty leaf: {site:08X}')
                    continue
                if entry <= site < entry+size or word >> 26 not in (16, 18):
                    continue
                bits = 16 if word >> 26 == 16 else 26
                delta = word & ((1 << bits)-4)
                if delta & (1 << (bits-1)):
                    delta -= 1 << bits
                target = ((0 if word & 2 else site) + delta) & 0xFFFFFFFF
                if entry <= target < entry+size:
                    if target != entry or not word & 1:
                        raise ValueError(f'interior or non-call edge to empty leaf: {site:08X}')
                    edges.append((site, target, word))
        edges.sort()
        actual = hashlib.sha256(b''.join(struct.pack('>III', *e) for e in edges)).hexdigest()
        if len(edges) != count or actual != edge_digest:
            raise ValueError(f'empty-leaf direct edges changed: {entry:08X}, {len(edges)}, {actual}')
        records.append({'entry': entry, 'bytes': size, 'body_sha256': digest,
                        'direct_calls': count, 'edges_sha256': actual,
                        'call_sites': [e[0] for e in edges]})
    return records


def pointer_lifetime(raw, records, functions, jump_tables):
    """The last leaf leaves a private-frame pointer in r0: forbid any later use."""
    image = Dol(raw)
    tracer = Tracer(image.word, lambda pc: image.contains_text(pc, 4),
                    read_jump_tables(jump_tables, FlowImage(raw)))
    with functions.open(newline='') as stream:
        bounds = [(int(r['addr_hex'], 16), int(r['size'])) for r in csv.DictReader(stream)]
    checked = 0
    for site in records[2]['call_sites']:
        contexts = [(a, a+s) for a, s in bounds if a <= site < a+s]
        if not contexts:
            raise ValueError(f'no original caller context at {site:08X}')
        for context in contexts:
            reads, barriers, escapes = tracer.trace(site+4, 'r0', context)
            if reads or barriers or escapes:
                raise ValueError(f'private-frame pointer survives in caller at {site:08X}: '
                                 f'{reads}, {barriers}, {escapes}')
            checked += 1
    if checked != 507:
        raise ValueError(f'private-frame pointer caller population changed: {checked}')
    return checked


def verify_retirement(generated, linker_map=None, shipping=False):
    def retired(address):
        return any(entry <= address < entry+size for entry, size, *_ in LEAVES)
    for path in [*generated.glob('recomp_*.cpp'), generated/'recomp_decls.h']:
        # Display names in the metadata table are not callable symbols.
        text = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', '', path.read_text(), flags=re.S)
        for found in re.finditer(r'\b(?:fn_|L_)([0-9A-Fa-f]{8})(?:_|\b)', text):
            if retired(int(found[1], 16)):
                raise ValueError(f'old generated body/interior remains: {path.name} {found[0]}')
    header = (generated/'recomp_decls.h').read_text()
    for entry, *_ in LEAVES:
        if f'void empty_variadic_{entry:08X}(Cpu& c);' not in header:
            raise ValueError('generated semantic entry is missing')
    if linker_map:
        text = linker_map.read_text()
        if shipping and '?kLiftSiteWords@' in text:
            raise ValueError('validation-only instruction words leaked into the shipping image')
        for found in re.finditer(r'\?fn_([0-9A-Fa-f]{8})(?:_|@)', text):
            if retired(int(found[1], 16)):
                raise ValueError(f'old linked body remains: {found[0]}')
        for entry, *_ in LEAVES:
            if f'?empty_variadic_{entry:08X}@' not in text:
                raise ValueError('linked semantic entry is missing')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dol', 'leaves', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--generator', type=Path)
    parser.add_argument('--functions', type=Path)
    parser.add_argument('--jump-tables', type=Path)
    parser.add_argument('--generated', type=Path)
    parser.add_argument('--map', type=Path)
    parser.add_argument('--shipping', action='store_true')
    args = parser.parse_args()
    raw = args.dol.read_bytes()
    records = audit(raw, args.leaves.read_text())
    checked = None
    if args.jump_tables:
        if not args.functions:
            raise ValueError('--jump-tables requires --functions')
        checked = pointer_lifetime(raw, records, args.functions, args.jump_tables)
    args.out.mkdir(parents=True, exist_ok=True)
    if args.generator:
        if not args.functions:
            raise ValueError('--generator requires the original --functions table for exact extents')
        only = args.out / 'only.txt'
        only.write_text(''.join(f'0x{entry:08X}\n' for entry, *_ in LEAVES))
        subprocess.run([str(args.generator.resolve()), '--dol', str(args.dol.resolve()),
                        '--functions', str(args.functions.resolve()), '--only', str(only.resolve()),
                        '--out', str(args.out.resolve()),
                        '--shards', '1', '--no-gap-sweep', '--no-wide-entries',
                        '--empty-variadic-leaves', str(args.leaves.resolve())], check=True)
        verify_retirement(args.out)
    if args.generated:
        verify_retirement(args.generated, args.map, args.shipping)
    elif args.map:
        raise ValueError('--map requires --generated')
    (args.out / 'contract.json').write_text(json.dumps({
        'dol_sha256': DOL_SHA256, 'leaves': records,
        'private_pointer_contexts_checked': checked,
        'scope': 'Original bodies, encoded external calls, aligned data references and optional direct-flow pointer lifetime.'}, indent=2)+'\n')
    print(f'Pinned 3 empty variadic leaves, 941 direct calls; no aligned data or interior references. '
          f'Private-pointer contexts: {checked}.')


if __name__ == '__main__':
    main()
