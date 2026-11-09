#!/usr/bin/env python3
"""The user's request of 2026-11-05: the language names in the Options menu (Text Language and Voice values) are words
of the menu's language. English keeps the native names (what every menu language showed so far, and what the other
five still get through the English fallback); Hungarian names them in Hungarian. Keys lang.<choice key>."""
import json
import os

I18N = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'i18n')
#
# Second request the same day ("the Japanese text issue is still not resolved": the English menu's Voice row showed
# 日本語): every menu language names the languages in its own words.
NAMES = {
    'en': {'lang.jp': 'Japanese', 'lang.en': 'English', 'lang.fr': 'French', 'lang.sp': 'Spanish',
           'lang.ge': 'German', 'lang.it': 'Italian', 'lang.hu': 'Hungarian'},
    'fr': {'lang.jp': 'Japonais', 'lang.en': 'Anglais', 'lang.fr': 'Français', 'lang.sp': 'Espagnol',
           'lang.ge': 'Allemand', 'lang.it': 'Italien', 'lang.hu': 'Hongrois'},
    'ge': {'lang.jp': 'Japanisch', 'lang.en': 'Englisch', 'lang.fr': 'Französisch', 'lang.sp': 'Spanisch',
           'lang.ge': 'Deutsch', 'lang.it': 'Italienisch', 'lang.hu': 'Ungarisch'},
    'it': {'lang.jp': 'Giapponese', 'lang.en': 'Inglese', 'lang.fr': 'Francese', 'lang.sp': 'Spagnolo',
           'lang.ge': 'Tedesco', 'lang.it': 'Italiano', 'lang.hu': 'Ungherese'},
    'sp': {'lang.jp': 'Japonés', 'lang.en': 'Inglés', 'lang.fr': 'Francés', 'lang.sp': 'Español',
           'lang.ge': 'Alemán', 'lang.it': 'Italiano', 'lang.hu': 'Húngaro'},
    'jp': {'lang.jp': '日本語', 'lang.en': '英語', 'lang.fr': 'フランス語', 'lang.sp': 'スペイン語',
           'lang.ge': 'ドイツ語', 'lang.it': 'イタリア語', 'lang.hu': 'ハンガリー語'},
    'hu': {'lang.jp': 'Japán', 'lang.en': 'Angol', 'lang.fr': 'Francia', 'lang.sp': 'Spanyol',
           'lang.ge': 'Német', 'lang.it': 'Olasz', 'lang.hu': 'Magyar'},
}

for lang, names in NAMES.items():
    path = os.path.join(I18N, 'strings_%s.json' % lang)
    raw = open(path, 'rb').read().decode('utf-8')
    data = json.loads(raw)
    add = {k: v for k, v in names.items() if k not in data}
    if add:
        body = raw.rstrip()
        assert body.endswith('}'), path
        body = body[:-1].rstrip()
        lines = ',\n'.join('  %s: %s' % (json.dumps(k), json.dumps(v, ensure_ascii=False)) for k, v in add.items())
        raw = body + ',\n' + lines + '\n}\n'
    for k, v in names.items():
        if data.get(k, v) != v:
            raw = raw.replace('%s: %s' % (json.dumps(k), json.dumps(data[k], ensure_ascii=False)),
                              '%s: %s' % (json.dumps(k), json.dumps(v, ensure_ascii=False)), 1)
    check = json.loads(raw)
    assert all(check[k] == v for k, v in names.items()), lang
    open(path, 'wb').write(raw.encode('utf-8'))
    print(lang, 'ok', len(add), 'added')
