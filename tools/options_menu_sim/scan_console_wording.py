#!/usr/bin/env python3
"""Lists every message of the game's .fnt text tables that still speaks of console hardware (memory card, slot,
GameCube, power / reset buttons), per language. Read-only.

    python tools/options_menu_sim/scan_console_wording.py [game files dir] > report.txt
"""
import glob
import os
import re
import struct
import sys

FILES = sys.argv[1] if len(sys.argv) > 1 else r'H:/ShadowTheHedgehogDiskGC_Extract/game/files'
PATTERNS = re.compile(
    r'memory\s*card|carte\s*m[ée]moire|speicherkarte|scheda\s*di\s*memoria|tarjeta\s*de\s*memoria|'
    r'memorycard|slot\s*[ab]|steckplatz|fente|ranura|alloggiamento|gamecube|game\s*cube|'
    r'メモリーカード|スロット|ゲームキューブ|power\s*button|reset|nintendo',
    re.IGNORECASE)


def messages(path):
    d = open(path, 'rb').read()
    if len(d) < 4:
        return
    n = struct.unpack('<I', d[:4])[0]
    if n == 0 or 4 + 20 * n > len(d):
        return
    for i in range(n):
        off, key = struct.unpack('<2I', d[4 + 20 * i:12 + 20 * i])
        if off >= len(d):
            continue
        e = off
        while e + 1 < len(d) and d[e:e + 2] != b'\x00\x00':
            e += 2
        yield key // 1000, d[off:e].decode('utf-16-le', 'replace')


def main():
    hits = {}
    for path in sorted(glob.glob(os.path.join(FILES, 'fonts', '**', '*.fnt'), recursive=True)):
        name = os.path.basename(path)
        m = re.search(r'_([A-Z]{2})\.fnt$', name)
        lang = m.group(1) if m else '??'
        for mid, text in messages(path):
            if PATTERNS.search(text):
                hits.setdefault(lang, []).append((os.path.relpath(path, FILES), mid, text.replace('\n', ' / ')))
    for lang in sorted(hits):
        print('== %s: %d messages' % (lang, len(hits[lang])))
        for f, mid, text in hits[lang]:
            print('  %s #%d: %s' % (f, mid, text))


if __name__ == '__main__':
    main()
