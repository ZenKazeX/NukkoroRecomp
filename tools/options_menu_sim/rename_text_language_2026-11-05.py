#!/usr/bin/env python3
"""The user's decision of 2026-11-05: the "Subtitles" row (audio.subtitles, the game's text language) is named a text
language row in every language; the "Show subtitles" switch stays. Ids are unchanged (options.cfg compatibility)."""
import json
import os

I18N = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'i18n')
LABELS = {
    'en': 'Text language',
    'fr': 'Langue du texte',
    'ge': 'Textsprache',
    'it': 'Lingua dei testi',
    'sp': 'Idioma del texto',
    'jp': 'テキスト言語',
    'hu': 'Szöveg nyelve',
}
EN_HELP = "Change the language of all the game's text,\nsubtitles included."

for lang, label in LABELS.items():
    path = os.path.join(I18N, 'strings_%s.json' % lang)
    raw = open(path, 'rb').read().decode('utf-8')
    data = json.loads(raw)
    old_label = data['audio.subtitles.label']
    raw = raw.replace('"audio.subtitles.label": %s' % json.dumps(old_label, ensure_ascii=False),
                      '"audio.subtitles.label": %s' % json.dumps(label, ensure_ascii=False), 1)
    if lang == 'en':
        old_help = data['audio.subtitles.help']
        raw = raw.replace('"audio.subtitles.help": %s' % json.dumps(old_help, ensure_ascii=False),
                          '"audio.subtitles.help": %s' % json.dumps(EN_HELP, ensure_ascii=False), 1)
    check = json.loads(raw)
    assert check['audio.subtitles.label'] == label, lang
    open(path, 'wb').write(raw.encode('utf-8'))
    print(lang, repr(old_label), '->', repr(label))
