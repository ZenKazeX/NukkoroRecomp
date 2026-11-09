#!/usr/bin/env python3
"""Checks a PC save-message translation (pc_save_<lang>.json) against the language's save font and the layout rules.

    python tools/options_menu_sim/save_text/check_save.py FR

Rules: the 29 GameCube-set ids (80-88, 90-101, 103-106, 108, 110, 113, 125) are all present; every character is in
glyphs_save_<LANG>.txt (newline aside); no line is longer than the longest line of the original message in that language
(or 40 characters if that is longer; Japanese: 21); no more lines than the original + 1 (at least 3); no console wording
is left (Memory Card, Slot, POWER, GameCube, format...).
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
IDS = [80, 81, 82, 83, 84, 85, 86, 87, 88, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 103, 104, 105, 106, 108,
       110, 113, 125]
CONSOLE = re.compile(r'memory|slot|power|gamecube|steckplatz|fente|ranura|alloggiamento|メモリーカード|スロット|'
                     r'ゲームキューブ|ＰＯＷＥＲ|POWERボタン|電源', re.IGNORECASE)


def main():
    lang = sys.argv[1].upper()
    glyphs = set(open(os.path.join(HERE, 'glyphs_save_%s.txt' % lang), encoding='utf-8').read())
    original = json.load(open(os.path.join(HERE, 'save_originals.json'), encoding='utf-8'))[lang]
    tr = json.load(open(os.path.join(HERE, 'pc_save_%s.json' % lang.lower()), encoding='utf-8'))
    problems = []
    for i in IDS:
        key = str(i)
        if key not in tr:
            problems.append('%s: missing' % key)
            continue
        text = tr[key]
        orig = original[key]
        lines = text.split('\n')
        width = 21 if lang == 'JP' else max(40, max(len(l) for l in orig.split('\n')))
        for line in lines:
            if len(line) > width:
                problems.append('%s: line of %d characters (max %d): %r' % (key, len(line), width, line))
        if len(lines) > max(3, len(orig.rstrip('\n').split('\n')) + 1):
            problems.append('%s: %d lines (original %d)' % (key, len(lines), len(orig.rstrip('\n').split('\n'))))
        bad = sorted(set(c for c in text if c != '\n' and c not in glyphs))
        if bad:
            problems.append('%s: characters not in the save font: %s' % (key, ' '.join(repr(c) for c in bad)))
        if CONSOLE.search(text):
            problems.append('%s: console wording left: %r' % (key, text))
    for p in problems:
        print(p)
    print('pc_save_%s.json: %s' % (lang.lower(), 'all %d ids pass' % len(IDS) if not problems else '%d problem(s)' % len(problems)))
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
