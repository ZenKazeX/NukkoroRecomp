#!/usr/bin/env python3
"""Brings strings_hu.json (from the Hungarian subtitle work, .build/hu-loc) to the menu table's rules: " / " line
separators become real line breaks, labels and words fit the row (22 / 30 characters), help lines fit 44, and the keys
added after it was made are translated. Run check_strings.py HU afterwards.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

FIXES = {
    'graphics.render_scale.label': "Belső felbontás",
    'controls.invert_camera_x.label': "Kamera X fordított",
    'controls.invert_camera_y.label': "Kamera Y fordított",
    'system.restore_all.label': "Minden visszaállítása",
    'word.confirm.question': "Megtartod a beállításokat?",
    'controls.vibration.help': "A kontroller rezgésének be- vagy\nkikapcsolása.",
    'controls.rumble_strength.help': "A kontroller rezgésének erősségét\nállítja be.",
    'gameplay.autosave.help': "Az automatikus mentés be- vagy\nkikapcsolása.",
    'display.mode.help': "Ablakos, teljes képernyős vagy exkluzív\nmód. 15 másodperc alatt erősítsd meg.",
    'display.aspect.help': "Automatikus módban a kép kitölti a\nképernyőt, rögzítve fekete sávok lehetnek.",
    'display.fps_counter.help': "A képkockasebességet mutatja a képernyő\nsarkában.",
    'graphics.render_scale.help': "A 3D-s kép belső felbontását módosítja,\na képernyő és a HUD felbontása marad.",
    'graphics.anisotropy.help': "Az anizotropikus szűrés élesen tartja a\nferdén nézett felületek textúráit.",
    'controls.invert_camera_x.help': "Megfordítja a kamerakar vízszintes\nirányát.",
    'controls.invert_camera_y.help': "Megfordítja a kamerakar függőleges\nirányát.",
    'gameplay.load_game.help': "Játékállás betöltése. A haladás és a\nmódosítások automatikusan mentődnek.",
    'system.restore_all.help': "A menü összes beállítását visszaállítja,\na nyelvi beállítások kivételével.",
    'menu.confirm.help': "Megtartod az új megjelenítési beállításokat?\nLejárat után a korábbiak térnek vissza.",
    # added after the Hungarian file was made
    'audio.music_volume.label': "Zene hangereje",
    'audio.music_volume.help': "A zene hangerejét állítja be.",
    'audio.sfx_volume.label': "Effektek hangereje",
    'audio.sfx_volume.help': "A hangeffektek hangerejét állítja be.",
    'audio.voice_volume.label': "Szinkron hangereje",
    'audio.voice_volume.help': "A szereplők hangjának hangerejét állítja\nbe.",
    'audio.subtitles_shown.label': "Feliratok",
    'audio.subtitles_shown.help': "A beszéd feliratainak megjelenítése vagy\nelrejtése.",
    'word.restart': "(újraindítás)",
}


def main():
    path = os.path.join(HERE, 'strings_hu.json')
    data = json.load(open(path, encoding='utf-8'))
    data = {k: v.replace(' / ', '\n') for k, v in data.items()}
    data.update(FIXES)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write('\n')
    print('hu: %d keys, %d fixes' % (len(data), len(FIXES)))


if __name__ == '__main__':
    main()
