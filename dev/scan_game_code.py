"""Proves a set of files carries no game code and no disc data (the release package's prebuilt parts).

    python dev/scan_game_code.py --disc <extracted PAL disc> [--dumpbin <dumpbin.exe>] [--json <report>] <file or dir>...

Three checks, every file of the given folders:
  1. Byte runs shared with the disc: every 32-byte window of every disc file (sys/main.dol, sys/*.bin, the whole files/
     tree), at every offset, is looked up among the scanned files' windows (taken every 16 bytes, so any shared run of
     47 bytes or more is found). The scanned files are indexed as they are and byte-swapped in 16- and 32-bit units at
     every alignment (the console is big-endian: a table copied into x64 code is usually swapped). Windows with fewer
     than 8 distinct byte values (padding, fills) are not indexed. Every hit is verified byte for byte and grown to its
     full length. A run whose bytes are an arithmetic sequence of 8-, 16- or 32-bit numbers (0, 1, 2, 3, ...: identity
     and counting tables any program has) is reported as trivial, and one whose bytes occur as they are in a
     --toolchain-reference file (the compiler's and the Windows SDK's C runtime libraries, which every program they link
     carries: case tables, and so on) as toolchain.
  2. Translated game functions: the recompiler names each translated guest function fn_XXXXXXXX (its address). With
     --dumpbin, every archive's symbol table is read and any DEFINED symbol naming fn_8xxxxxxx is a failure (an
     undefined one is only a reference to code the player's build provides; a string literal that mentions the name,
     ??_C@..., is listed apart).
  3. Personal paths: the user name and profile folder of the machine that ran the release step must not appear.
Exit code 0 when: no other (real) shared run of 64 bytes or more, no file with 256 or more real shared bytes in
all, no defined translated function, no personal path.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

W = 32            # window length
STRIDE = 16       # index stride in the scanned files
MIN_DISTINCT = 8
FAIL_RUN = 64     # a verified non-trivial shared run this long fails the scan
FAIL_TOTAL = 256  # so do this many non-trivial shared bytes in one scanned file
REFERENCES = {}   # Path -> bytes: --toolchain-reference files
K = np.array([0x9E3779B97F4A7C15, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5], dtype=np.uint64)


def window_hashes(data: np.ndarray, offsets: np.ndarray) -> np.ndarray:
    """64-bit hash of data[o:o+W] for each offset (data padded by W bytes)."""
    # Every byte offset as a little-endian 64-bit word (an unaligned, 1-byte-stride view; nothing is copied).
    words = np.ndarray((len(data) - 7,), dtype='<u8', buffer=data, strides=(1,))
    h = np.zeros(len(offsets), dtype=np.uint64)
    for part in range(4):
        word = words[offsets + part * 8]
        with np.errstate(over='ignore'):
            h ^= word * K[part]
            h = (h << np.uint64(23)) | (h >> np.uint64(41))
    return h


def distinct_ok(data: np.ndarray, offsets: np.ndarray) -> np.ndarray:
    """Windows worth indexing: 8 or more distinct byte values, and not an arithmetic sequence of 8-, 16- or 32-bit
    numbers (counting and identity tables, found in any program and in audio data alike)."""
    rows = data[offsets[:, None] + np.arange(W)[None, :]]
    srt = np.sort(rows, axis=1)
    ok = (1 + (np.diff(srt, axis=1) != 0).sum(axis=1)) >= MIN_DISTINCT
    r64 = rows.astype(np.uint64)
    for width in (1, 2, 4):
        mod = np.uint64((1 << (8 * width)) - 1)
        for phase in range(width):
            count = (W - phase) // width
            cols = r64[:, phase:phase + count * width].reshape(len(rows), count, width)
            for big in (False, True):
                v = np.zeros((len(rows), count), dtype=np.uint64)
                for b in range(width):
                    shift = np.uint64(8 * (width - 1 - b) if big else 8 * b)
                    v |= cols[:, :, b] << shift
                d = (v[:, 1:] - v[:, :-1]) & mod
                ok &= ~(d == d[:, :1]).all(axis=1)
    return ok


def all_files(paths):
    for p in paths:
        p = Path(p)
        if p.is_dir():
            yield from sorted(q for q in p.rglob('*') if q.is_file())
        elif p.is_file():
            yield p


def views(raw: bytes):
    """The file as it is, and byte-swapped in 16- and 32-bit units at every alignment: (name, phase, bytes)."""
    yield 'as is', 0, raw
    for width in (2, 4):
        for phase in range(width):
            body = raw[phase:]
            body = body[:len(body) - len(body) % width]
            a = np.frombuffer(body, dtype=np.uint8).reshape(-1, width)[:, ::-1]
            yield f'swap{8 * width}+{phase}', phase, a.tobytes()


def build_index(files):
    blobs, hashes, owners, offs = [], [], [], []  # blobs: (file number, view name, phase, bytes)
    for n, f in enumerate(files):
        for name, phase, raw in views(f.read_bytes()):
            b = len(blobs)
            blobs.append((n, name, phase, raw))
            if len(raw) < W:
                continue
            data = np.frombuffer(raw + bytes(W), dtype=np.uint8)
            o = np.arange(0, len(raw) - W + 1, STRIDE, dtype=np.int64)
            keep = np.zeros(len(o), dtype=bool)
            for s in range(0, len(o), 1 << 20):
                keep[s:s + (1 << 20)] = distinct_ok(data, o[s:s + (1 << 20)])
            o = o[keep]
            for s in range(0, len(o), 1 << 20):
                hashes.append(window_hashes(data, o[s:s + (1 << 20)]))
            owners.append(np.full(len(o), b, dtype=np.int32))
            offs.append(o)
    h = np.concatenate(hashes) if hashes else np.zeros(0, dtype=np.uint64)
    own = np.concatenate(owners) if owners else np.zeros(0, dtype=np.int32)
    off = np.concatenate(offs) if offs else np.zeros(0, dtype=np.int64)
    # One location per distinct window is enough to find and grow a shared run (archives repeat many windows).
    h, first = np.unique(h, return_index=True)
    return blobs, h, own[first], off[first]


def trivial(run: bytes) -> bool:
    """An arithmetic sequence of 8-, 16- or 32-bit numbers, either byte order, any phase."""
    for width in (1, 2, 4):
        for order in ('little', 'big'):
            for phase in range(width):
                body = run[phase:]
                vals = [int.from_bytes(body[i:i + width], order) for i in range(0, len(body) - width + 1, width)]
                if len(vals) >= 4:
                    mod = 1 << (8 * width)
                    step = (vals[1] - vals[0]) % mod
                    if all((vals[i + 1] - vals[i]) % mod == step for i in range(len(vals) - 1)):
                        return True
    return False


def grow(a: bytes, ai: int, b: bytes, bi: int):
    s = 0
    while ai - s - 1 >= 0 and bi - s - 1 >= 0 and a[ai - s - 1] == b[bi - s - 1]:
        s += 1
    e = 0
    while ai + e < len(a) and bi + e < len(b) and a[ai + e] == b[bi + e]:
        e += 1
    return ai - s, bi - s, s + e


def scan_disc(disc: Path, index, files, report):
    blobs, h, own, off = index
    found = {}
    disc_files = [p for p in sorted((disc / 'sys').glob('*')) if p.is_file()] + \
                 sorted(p for p in (disc / 'files').rglob('*') if p.is_file())
    total = 0
    chunk = 8 << 20
    for df in disc_files:
        raw = df.read_bytes()
        if (total + len(raw)) >> 28 != total >> 28:
            print(f'  ... {(total + len(raw)) >> 20} MB of the disc scanned', file=sys.stderr, flush=True)
        total += len(raw)
        if len(raw) < W or len(h) == 0:
            continue
        data = np.frombuffer(raw + bytes(W), dtype=np.uint8)
        for s in range(0, len(raw) - W + 1, chunk):
            o = np.arange(s, min(s + chunk, len(raw) - W + 1), dtype=np.int64)
            hh = window_hashes(data, o)
            pos = np.searchsorted(h, hh)
            pos[pos >= len(h)] = len(h) - 1
            covered = -1  # disc bytes before this are inside a run already grown
            for i in np.nonzero(h[pos] == hh)[0]:
                p = pos[i]
                fileno, view, phase, blob = blobs[own[p]]
                bi, di = int(off[p]), int(o[i])
                if di < covered or blob[bi:bi + W] != raw[di:di + W]:
                    continue
                b0, d0, n = grow(blob, bi, raw, di)
                covered = d0 + n - W + 1
                key = (int(own[p]), b0)
                if key not in found or found[key]['length'] < n:
                    run = blob[b0:b0 + n]
                    found[key] = {'file': str(files[fileno]), 'view': view, 'offset': phase + b0, 'length': n,
                                  'disc_file': str(df.relative_to(disc)), 'disc_offset': d0,
                                  'trivial': trivial(run),
                                  'toolchain': [r.name for r in REFERENCES if run in REFERENCES[r]],
                                  'text': blob[b0:b0 + min(n, 96)].decode('latin-1').encode('unicode_escape').decode()}
    report['disc_bytes_scanned'] = total
    report['disc_files_scanned'] = len(disc_files)
    report['shared_runs'] = sorted(found.values(), key=lambda r: -r['length'])
    return report


def translated_symbols(files, dumpbin, report):
    defined, literals, undefined = [], [], 0
    name_re = re.compile(r'fn_8[0-9A-Fa-f]{7}')
    for f in files:
        if f.suffix.lower() not in ('.lib', '.obj'):
            continue
        out = subprocess.run([dumpbin, '/symbols', str(f)], capture_output=True, text=True, errors='replace').stdout
        for line in out.splitlines():
            if '|' not in line or not name_re.search(line):
                continue
            symbol = line.split('|', 1)[1].strip().split(' ', 1)[0]
            if not name_re.search(symbol):
                continue
            if ' UNDEF ' in line:
                undefined += 1
            elif symbol.startswith('??_C@'):
                literals.append(f'{f.name}: {symbol}')
            else:
                defined.append(f'{f.name}: {symbol}')
    report['translated_functions_defined'] = sorted(set(defined))
    report['translated_function_name_strings'] = sorted(set(literals))
    report['translated_function_references'] = undefined
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', required=True)
    ap.add_argument('--dumpbin')
    ap.add_argument('--json')
    ap.add_argument('--toolchain-reference', action='append', default=[])
    ap.add_argument('paths', nargs='+')
    args = ap.parse_args()
    for ref in args.toolchain_reference:
        REFERENCES[Path(ref)] = Path(ref).read_bytes()
    files = list(all_files(args.paths))
    report = {'files': [{'path': str(f), 'size': f.stat().st_size} for f in files],
              'window': W, 'stride': STRIDE, 'min_distinct': MIN_DISTINCT, 'fail_run': FAIL_RUN, 'fail_total': FAIL_TOTAL}
    index = build_index(files)
    report['indexed_windows'] = int(len(index[1]))
    scan_disc(Path(args.disc), index, files, report)
    if args.dumpbin:
        translated_symbols(files, args.dumpbin, report)
    personal = [s for s in {os.environ.get('USERNAME', ''), os.environ.get('USERPROFILE', '')} if len(s) >= 3]
    hits = []
    for fileno, view, _, blob in index[0]:
        if view != 'as is':
            continue
        for s in personal:
            for enc in ('utf-8', 'utf-16-le'):
                if s.encode(enc) in blob or s.lower().encode(enc) in blob.lower():
                    hits.append(f'{files[fileno].name} ({enc})')
    report['personal_path_hits'] = hits
    report['toolchain_references'] = [str(r) for r in REFERENCES]
    real = [r for r in report['shared_runs'] if not r['trivial'] and not r['toolchain']]
    long_runs = [r for r in real if r['length'] >= FAIL_RUN]
    per_file = {}
    for r in real:
        per_file[r['file']] = per_file.get(r['file'], 0) + r['length']
    heavy = {k: v for k, v in per_file.items() if v >= FAIL_TOTAL}
    report['real_shared_bytes_per_file'] = per_file
    report['ok'] = not long_runs and not heavy and not report.get('translated_functions_defined') and not hits
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding='utf-8')
    print(f"{len(files)} files, {sum(f['size'] for f in report['files'])} bytes; {report['indexed_windows']} windows indexed "
          f"(7 views each); {report['disc_files_scanned']} disc files / {report['disc_bytes_scanned']} bytes scanned")
    print(f"shared runs >= {W} bytes: {len(report['shared_runs'])} ({len(real)} real, longest "
          f"{max((r['length'] for r in real), default=0)}); real >= {FAIL_RUN}: {len(long_runs)}; "
          f"files with >= {FAIL_TOTAL} real shared bytes: {len(heavy)}")
    for r in report['shared_runs'][:40]:
        kind = 'trivial  ' if r['trivial'] else 'toolchain' if r['toolchain'] else 'REAL     '
        print(f"  {r['length']:6d} {kind} {Path(r['file']).name}+{r['offset']:#x} "
              f"({r['view']})  {r['disc_file']}+{r['disc_offset']:#x}  {r['text'][:60]}")
    for k, v in per_file.items():
        print(f"  real shared bytes in {Path(k).name}: {v}")
    if args.dumpbin:
        print(f"translated functions defined: {report['translated_functions_defined']}; "
              f"references (undefined): {report['translated_function_references']}; "
              f"string literals naming one: {report['translated_function_name_strings']}")
    print(f"personal path hits: {hits}")
    print('OK' if report['ok'] else 'FAILED')
    sys.exit(0 if report['ok'] else 1)


if __name__ == '__main__':
    main()
