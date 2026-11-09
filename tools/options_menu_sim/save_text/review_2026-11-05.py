#!/usr/bin/env python3
"""Review fixes of the agents' PC save wording (2026-11-05): menu item 113 shortened to fit its button, German "Sie"
throughout and two stiff phrases, Spanish accent, Italian 87 rephrased."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIXES = {
    'fr': {'113': 'Nouvelle sauvegarde'},
    'it': {'113': 'Nuovo salvataggio',
           '87': 'Se crei un nuovo file di salvataggio,\nverranno cancellati tutti i progressi\nsalvati. Vuoi continuare?'},
    'sp': {'113': 'Crear archivo nuevo',
           '81': 'No se ha podido crear el archivo\nde guardado. Si continúas, se\ndesactivará la función de autoguardado.',
           '94': 'No se ha podido crear el archivo\nde guardado. Si continúas, se\ndesactivará la función de autoguardado.'},
    'ge': {'81': 'Der Spielstand konnte nicht erstellt\nwerden. Wenn Sie fortfahren, ist das\nautomatische Speichern deaktiviert.',
           '94': 'Der Spielstand konnte nicht erstellt\nwerden. Wenn Sie fortfahren, ist das\nautomatische Speichern deaktiviert.',
           '80': 'Speichervorgang läuft.\nBitte beenden Sie das Spiel nicht und\nschalten Sie den PC nicht aus.',
           '106': 'Speichervorgang läuft.\nBitte beenden Sie das Spiel nicht und\nschalten Sie den PC nicht aus.',
           '110': 'Laden fehlgeschlagen.\nBitte prüfen Sie den Spielstand\nund versuchen Sie es erneut.'},
}
for lang, fixes in FIXES.items():
    path = os.path.join(HERE, 'pc_save_%s.json' % lang)
    data = json.load(open(path, encoding='utf-8'))
    data.update(fixes)
    with open(path, 'wb') as f:
        f.write(json.dumps(data, ensure_ascii=False, indent=1).encode('utf-8'))
    print(lang, len(fixes), 'fixed')
