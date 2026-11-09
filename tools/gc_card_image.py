"""Read a raw GameCube memory-card image: the header, both directory and
block-allocation-table copies (which one is current, checksums), and the file
list. Stage 5 (docs/native-time-20261005/STAGE5_EXI.md, planned) uses it to
verify that native card writes keep the image a valid card the CARD library -
and the user's other tools - can read.

Layout (8 KiB blocks, big-endian): block 0 header (checksums over 0x000-0x1FB
at 0x1FC); blocks 1-2 directory copies (127 entries of 64 bytes, then an
update counter at 0x1FFA and checksums at 0x1FFC); blocks 3-4 allocation
tables (checksums at 0x0000, update counter 0x0004, free blocks 0x0006, last
allocated 0x0008, then one u16 per block from block 5: 0x0000 free, 0xFFFF
last block of a file, else the next block). The checksum pair is the 16-bit
sum of the region's u16 words and the sum of their complements (the CARD
library's __CARDCheckSum). The copy with the higher update counter (and valid
checksums) is current.

usage: gc_card_image.py <card.raw> [--json]
       gc_card_image.py --make-empty <source card.raw> <new card.raw>
         writes a NEW file (refuses to overwrite; never writes the source): the
         source's header (serial, format time, size, encoding) with an empty
         directory and every data block free in both copies, checksums
         recomputed and the update counters advanced - a freshly formatted
         card of the same identity, for routes that must create a save.
"""
from pathlib import Path
import json
import struct
import sys

BLOCK = 0x2000
DIR_ENTRIES = 127
ENTRY = 0x40
FREE_ENTRY = bytes([0xFF]) * ENTRY


def checksum(data):
    total = 0
    complement = 0
    for (word,) in struct.iter_unpack('>H', data):
        total = (total + word) & 0xFFFF
        complement = (complement + (word ^ 0xFFFF)) & 0xFFFF
    if total == 0xFFFF:
        total = 0
    if complement == 0xFFFF:
        complement = 0
    return total, complement


def read_directory(block):
    entries = []
    for index in range(DIR_ENTRIES):
        raw = block[index * ENTRY:(index + 1) * ENTRY]
        if raw[:4] == bytes([0xFF]) * 4:
            continue
        game, maker = raw[0:4].decode('latin-1'), raw[4:6].decode('latin-1')
        name = raw[8:40].split(bytes([0]), 1)[0].decode('latin-1')
        modified, image_offset = struct.unpack_from('>II', raw, 0x28)
        icon_format, icon_speed = struct.unpack_from('>HH', raw, 0x30)
        permissions, copies = raw[0x34], raw[0x35]
        first, length = struct.unpack_from('>HH', raw, 0x36)
        comment_offset = struct.unpack_from('>I', raw, 0x3C)[0]
        entries.append(dict(slot=index, game=game, maker=maker, name=name, banner_format=raw[7],
                            modified=modified, image_offset=image_offset, icon_format=icon_format,
                            icon_speed=icon_speed, permissions=permissions, copies=copies,
                            first_block=first, blocks=length, comment_offset=comment_offset))
    counter = struct.unpack_from('>H', block, 0x1FFA)[0]
    stored = struct.unpack_from('>HH', block, 0x1FFC)
    return dict(entries=entries, update_counter=counter, checksum_ok=checksum(block[:0x1FFC]) == stored)


def read_bat(block, total_blocks):
    stored = struct.unpack_from('>HH', block, 0)
    counter, free, last = struct.unpack_from('>HHH', block, 4)
    chain = list(struct.unpack_from('>%dH' % (total_blocks - 5), block, 0x0A))
    return dict(update_counter=counter, free_blocks=free, last_allocated=last,
                checksum_ok=checksum(block[4:0x2000]) == stored, chain=chain,
                free_in_chain=sum(1 for value in chain if value == 0))


def describe(path):
    data = Path(path).read_bytes()
    if len(data) % BLOCK or len(data) < 5 * BLOCK:
        raise SystemExit(f'{path}: not a whole number of 8 KiB blocks ({len(data)} bytes)')
    total_blocks = len(data) // BLOCK
    header = data[:BLOCK]
    size_mbits, encoding = struct.unpack_from('>HH', header, 0x22)
    header_ok = checksum(header[:0x1FC]) == struct.unpack_from('>HH', header, 0x1FC)
    directories = [read_directory(data[BLOCK * i:BLOCK * (i + 1)]) for i in (1, 2)]
    bats = [read_bat(data[BLOCK * i:BLOCK * (i + 1)], total_blocks) for i in (3, 4)]

    def current(copies):
        valid = [i for i, c in enumerate(copies) if c['checksum_ok']]
        if not valid:
            return None
        return max(valid, key=lambda i: copies[i]['update_counter'])

    d_index, b_index = current(directories), current(bats)
    files = []
    if d_index is not None and b_index is not None:
        chain = bats[b_index]['chain']
        for entry in directories[d_index]['entries']:
            blocks, block, seen = [], entry['first_block'], set()
            while 5 <= block < total_blocks and block not in seen and len(blocks) < total_blocks:
                seen.add(block)
                blocks.append(block)
                block = chain[block - 5]
                if block == 0xFFFF:
                    break
            files.append(dict(entry, chain_blocks=len(blocks), chain_ok=len(blocks) == entry['blocks']))
    return dict(path=str(path), bytes=len(data), blocks=total_blocks, size_mbits=size_mbits, encoding=encoding,
                header_checksum_ok=header_ok,
                directory=[dict(update_counter=c['update_counter'], checksum_ok=c['checksum_ok'],
                                files=len(c['entries'])) for c in directories],
                bat=[dict(update_counter=c['update_counter'], checksum_ok=c['checksum_ok'],
                          free_blocks=c['free_blocks'], free_in_chain=c['free_in_chain']) for c in bats],
                current_directory=None if d_index is None else d_index + 1,
                current_bat=None if b_index is None else b_index + 3, files=files)


def make_empty(source, target):
    target = Path(target)
    if target.exists():
        raise SystemExit(f'{target} exists; refusing to overwrite')
    report = describe(source)
    if not report['header_checksum_ok'] or report['current_directory'] is None or report['current_bat'] is None:
        raise SystemExit(f'{source}: not a valid card to start from')
    data = bytearray(Path(source).read_bytes())
    total_blocks = report['blocks']
    for index, copy in ((1, report['directory'][0]), (2, report['directory'][1])):
        start = BLOCK * index
        data[start:start + DIR_ENTRIES * ENTRY] = FREE_ENTRY * DIR_ENTRIES
        struct.pack_into('>H', data, start + 0x1FFA, (copy['update_counter'] + 2) & 0xFFFF)
        struct.pack_into('>HH', data, start + 0x1FFC, *checksum(bytes(data[start:start + 0x1FFC])))
    for index, copy in ((3, report['bat'][0]), (4, report['bat'][1])):
        start = BLOCK * index
        struct.pack_into('>HHH', data, start + 4, (copy['update_counter'] + 2) & 0xFFFF, total_blocks - 5, 4)
        data[start + 0x0A:start + 0x0A + 2 * (total_blocks - 5)] = bytes(2 * (total_blocks - 5))
        struct.pack_into('>HH', data, start, *checksum(bytes(data[start + 4:start + BLOCK])))
    data[BLOCK * 5:] = bytes(BLOCK * (total_blocks - 5))
    with target.open('xb') as stream:
        stream.write(bytes(data))
    check = describe(target)
    assert all(d['checksum_ok'] for d in check['directory']) and all(b['checksum_ok'] for b in check['bat'])
    assert not check['files'] and all(b['free_blocks'] == total_blocks - 5 for b in check['bat'])
    print(f'wrote an empty card {target} ({total_blocks} blocks, {total_blocks - 5} free)')
    return 0


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == '--make-empty':
        if len(argv) != 3:
            print(__doc__)
            return 2
        return make_empty(argv[1], argv[2])
    report = describe(argv[0])
    if '--json' in argv[1:]:
        print(json.dumps(report, indent=1))
        return 0
    print(f"{report['path']}: {report['blocks']} blocks ({report['size_mbits']} Mbit), encoding {report['encoding']}, "
          f"header checksum {'ok' if report['header_checksum_ok'] else 'BAD'}")
    for i, d in enumerate(report['directory'], 1):
        print(f"  directory copy {i}: counter {d['update_counter']}, checksum {'ok' if d['checksum_ok'] else 'BAD'}, "
              f"{d['files']} files")
    for i, b in enumerate(report['bat'], 3):
        print(f"  allocation table {i}: counter {b['update_counter']}, checksum {'ok' if b['checksum_ok'] else 'BAD'}, "
              f"{b['free_blocks']} free ({b['free_in_chain']} free in the chain)")
    print(f"  current: directory block {report['current_directory']}, allocation table block {report['current_bat']}")
    for f in report['files']:
        print(f"    [{f['slot']:3}] {f['game']}{f['maker']} {f['name']!r}: {f['blocks']} blocks from {f['first_block']} "
              f"(chain {'ok' if f['chain_ok'] else 'BROKEN'}), modified {f['modified']}, permissions {f['permissions']:#x}")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
