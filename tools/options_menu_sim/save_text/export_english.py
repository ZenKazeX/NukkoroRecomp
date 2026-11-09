#!/usr/bin/env python3
"""Exports pc_english_messages() from runtime/src/gekko/native_save_text.cpp as pc_save_en.json (the reference the
other languages' PC save wording is written from). One-time helper of 2026-11-05."""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', '..', '..', 'runtime', 'src', 'gekko', 'native_save_text.cpp')

src = open(SRC, encoding='utf-8').read()
body = src[src.index('pc_english_messages()'):src.index('return messages;')]
messages = {}
for m in re.finditer(r'\{(\d+),\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', body):
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
    messages[m.group(1)] = ''.join(parts).replace('\\n', '\n')
with open(os.path.join(HERE, 'pc_save_en.json'), 'wb') as f:
    f.write(json.dumps(messages, ensure_ascii=False, indent=1).encode('utf-8'))
print(len(messages), 'messages')
