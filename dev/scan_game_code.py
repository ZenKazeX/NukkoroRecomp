"""Proves a set of files carries no game code and no disc data (the release package's prebuilt parts).

    python dev/scan_game_code.py --disc <extracted PAL disc> [--dumpbin <dumpbin.exe>] [--json <report>] <file or dir>...

Three checks, every file of the given folders:
  1. Byte runs shared with the disc: every 32-byte window of every disc file (sys/main.dol, sys/*.bin, the whole files/
     tree), at every offset, is looked up among the scanned files' windows (taken every 16 bytes, so any shared run of
     47 bytes or more is found). Windows with fewer than 8 distinct byte values (padding, fills) are not indexed. Every
     hit is verified byte for byte and grown to its full length; the report lists each with the bytes as text.
  2. Translated game functions: the recompiler names each translated guest function fn_XXXXXXXX (its address). With
     --dumpbin, every archive's and program's symbol table is read and any DEFINED fn_8xxxxxxx symbol is a failure
     (an undefined one is only a reference to code the player's build provides).
  3. Personal paths: the user name and profile folder of the machine that ran the release step must not appear.
Exit code 0: no disc run of 64 bytes or more, no defined translated function, no personal path.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

W = 32          # window length
STRIDE = 16     # index stride in the scanned files
MIN_DISTINCT = 8
FAIL_RUN = 64   # a verified shared run this long fails the scan
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
    rows = np.sort(data[offsets[:, None] + np.arange(W)[None, :]], axis=1)
    return (1 + (np.diff(rows, axis=1) != 0).sum(axis=1)) >= MIN_DISTINCT


def all_files(paths):
    for p in paths:
        p = Path(p)
        if p.is_dir():
            yield from sorted(q for q in p.rglob('*') if q.is_file())
        elif p.is_file():
            yield p


def build_index(files):
    blobs, hashes, owners, offs = [], [], [], []
    for n, f in enumerate(files):
        raw = f.read_bytes()
        blobs.append(raw)
        data = np.frombuffer(raw + bytes(W), dtype=np.uint8)
        if len(raw) < W:
            continue
        o = np.arange(0, len(raw) - W + 1, STRIDE, dtype=np.int64)
        keep = np.zeros(len(o), dtype=bool)
        for s in range(0, len(o), 1 << 20):
            keep[s:s + (1 << 20)] = distinct_ok(data, o[s:s + (1 << 20)])
        o = o[keep]
        for s in range(0, len(o), 1 << 20):
            hashes.append(window_hashes(data, o[s:s + (1 << 20)]))
        owners.append(np.full(len(o), n, dtype=np.int32))
        offs.append(o)
    h = np.concatenate(hashes) if hashes else np.zeros(0, dtype=np.uint64)
    own = np.concatenate(owners) if owners else np.zeros(0, dtype=np.int32)
    off = np.concatenate(offs) if offs else np.zeros(0, dtype=np.int64)
    order = np.argsort(h, kind='stable')
    return blobs, h[order], own[order], off[order]


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
        total += len(raw)
        if len(raw) < W or len(h) == 0:
            continue
        data = np.frombuffer(raw + bytes(W), dtype=np.uint8)
        for s in range(0, len(raw) - W + 1, chunk):
            o = np.arange(s, min(s + chunk, len(raw) - W + 1), dtype=np.int64)
            hh = window_hashes(data, o)
            pos = np.searchsorted(h, hh)
            pos[pos >= len(h)] = len(h) - 1
            for i in np.nonzero(h[pos] == hh)[0]:
                p = pos[i]
                while p < len(h) and h[p] == hh[i]:
                    blob = blobs[own[p]]
                    bi, di = int(off[p]), int(o[i])
                    if blob[bi:bi + W] == raw[di:di + W]:
                        b0, d0, n = grow(blob, bi, raw, di)
                        key = (int(own[p]), b0)
                        if key not in found or found[key]['length'] < n:
                            found[key] = {'file': str(files[own[p]]), 'offset': b0, 'length': n,
                                          'disc_file': str(df.relative_to(disc)), 'disc_offset': d0,
                                          'text': blob[b0:b0 + min(n, 96)].decode('latin-1').encode('unicode_escape').decode()}
                    p += 1
    report['disc_bytes_scanned'] = total
    report['disc_files_scanned'] = len(disc_files)
    report['shared_runs'] = sorted(found.values(), key=lambda r: -r['length'])
    return report


def translated_symbols(files, dumpbin, report):
    defined, undefined = [], 0
    pattern = re.compile(r'\b(SECT[0-9A-F]+|UNDEF)\b.*\b(fn_8[0-9A-Fa-f]{7}\w*)')
    for f in files:
        if f.suffix.lower() not in ('.lib', '.exe', '.obj'):
            continue
        if f.suffix.lower() == '.exe':
            text = f.read_bytes()
            hits = sorted(set(m.decode() for m in re.findall(rb'fn_8[0-9A-Fa-f]{7}\w*', text)))
            report.setdefault('program_strings', {})[str(f)] = hits[:20]
            continue
        out = subprocess.run([dumpbin, '/symbols', str(f)], capture_output=True, text=True, errors='replace').stdout
        for line in out.splitlines():
            m = pattern.search(line)
            if m:
                if m.group(1) == 'UNDEF':
                    undefined += 1
                else:
                    defined.append(f'{f.name}: {m.group(2)}')
    report['translated_functions_defined'] = defined
    report['translated_function_references'] = undefined
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disc', required=True)
    ap.add_argument('--dumpbin')
    ap.add_argument('--json')
    ap.add_argument('paths', nargs='+')
    args = ap.parse_args()
    files = list(all_files(args.paths))
    report = {'files': [{'path': str(f), 'size': f.stat().st_size} for f in files],
              'window': W, 'stride': STRIDE, 'min_distinct': MIN_DISTINCT, 'fail_run': FAIL_RUN}
    index = build_index(files)
    report['indexed_windows'] = int(len(index[1]))
    scan_disc(Path(args.disc), index, files, report)
    if args.dumpbin:
        translated_symbols(files, args.dumpbin, report)
    personal = [s for s in {os.environ.get('USERNAME', ''), os.environ.get('USERPROFILE', '')} if len(s) >= 3]
    hits = []
    for f, blob in zip(files, index[0]):
        for s in personal:
            for enc in ('utf-8', 'utf-16-le'):
                if s.encode(enc) in blob or s.lower().encode(enc) in blob.lower():
                    hits.append(f'{f.name} ({enc})')
    report['personal_path_hits'] = hits
    long_runs = [r for r in report['shared_runs'] if r['length'] >= FAIL_RUN]
    report['ok'] = not long_runs and not report.get('translated_functions_defined') and not hits
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding='utf-8')
    print(f"{len(files)} files, {sum(f['size'] for f in report['files'])} bytes; {report['indexed_windows']} windows indexed; "
          f"{report['disc_files_scanned']} disc files / {report['disc_bytes_scanned']} bytes scanned")
    print(f"shared runs >= {W} bytes: {len(report['shared_runs'])}, longest "
          f"{report['shared_runs'][0]['length'] if report['shared_runs'] else 0}; >= {FAIL_RUN}: {len(long_runs)}")
    for r in report['shared_runs'][:15]:
        print(f"  {r['length']:6d}  {Path(r['file']).name}+{r['offset']:#x}  {r['disc_file']}+{r['disc_offset']:#x}  {r['text'][:70]}")
    if args.dumpbin:
        print(f"translated functions defined: {len(report['translated_functions_defined'])}; "
              f"references (undefined): {report['translated_function_references']}")
    print(f"personal path hits: {hits}")
    print('OK' if report['ok'] else 'FAILED')
    sys.exit(0 if report['ok'] else 1)


if __name__ == '__main__':
    main()
