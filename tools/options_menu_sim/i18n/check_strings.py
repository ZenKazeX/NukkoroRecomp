#!/usr/bin/env python3
"""Checks a translation of the PC Options menu's texts against strings_en.json and the language's glyph sheets.

    python check_strings.py FR        # checks strings_fr.json

Rules: every key of strings_en.json is present; a help has at most 2 lines of at most 44 characters; a label at most
22 characters; a word at most 30; placeholders (%d) and the "A  " / "B  " prefixes are kept; every character of a help
is in the language's glyph sheet (glyphs_<LANG>.txt) - the game draws helps from that sheet alone; labels and words may
also use the English sheet (glyphs_EN.txt), which the menu falls back to.
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    lang = sys.argv[1].upper()
    en = json.load(open(os.path.join(HERE, 'strings_en.json'), encoding='utf-8'))
    tr = json.load(open(os.path.join(HERE, 'strings_%s.json' % lang.lower()), encoding='utf-8'))
    own = set(open(os.path.join(HERE, 'glyphs_%s.txt' % lang), encoding='utf-8').read())
    eng = set(open(os.path.join(HERE, 'glyphs_EN.txt'), encoding='utf-8').read())
    problems = []
    for key, text in en.items():
        if key.startswith('_'):
            continue
        if key not in tr:
            # lang.*: the language names; a menu language without its own keeps the English column's (native names)
            if not key.startswith('lang.'):
                problems.append('%s: missing' % key)
            continue
        t = tr[key]
        if not isinstance(t, str) or not t.strip():
            problems.append('%s: empty' % key)
            continue
        if key.endswith('.help'):
            lines = t.split('\n')
            if len(lines) > 2:
                problems.append('%s: %d lines (max 2)' % (key, len(lines)))
            for line in lines:
                if len(line) > (20 if lang == "JP" else 44):
                    problems.append("%s: a line of %d characters (max %d): %r" % (key, len(line), 20 if lang == "JP" else 44, line))
            bad = sorted(set(c for c in t if c not in own and c not in ' \n'))
            if bad:
                problems.append('%s: characters not in the %s sheet: %s' % (key, lang, ''.join(bad)))
        else:
            limit = 22 if key.endswith(".label") else 30
            if len(t) > limit:
                problems.append('%s: %d characters (max %d)' % (key, len(t), limit))
            bad = sorted(set(c for c in t.replace('%d', '') if c not in own and c not in eng and c != ' '))
            if bad:
                problems.append('%s: characters in neither the %s nor the English sheet: %s' % (key, lang, ''.join(bad)))
        if '%d' in text and '%d' not in t:
            problems.append('%s: the %%d placeholder is missing' % key)
        for prefix in ('A  ', 'B  '):
            if text.startswith(prefix) and not t.startswith(prefix):
                problems.append('%s: must start with %r' % (key, prefix))
    extra = [k for k in tr if k not in en]
    if extra:
        problems.append('keys not in strings_en.json: %s' % ', '.join(extra))
    if problems:
        print('\n'.join(problems))
        print('%d problem(s)' % len(problems))
        sys.exit(1)
    print('strings_%s.json: all %d keys pass' % (lang.lower(), len([k for k in en if not k.startswith('_')])))


if __name__ == '__main__':
    main()
