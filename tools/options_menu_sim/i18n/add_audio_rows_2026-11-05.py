#!/usr/bin/env python3
"""Adds the texts of the four Audio rows added on 2026-11-05 (Music / Sound effects / Voice volume sliders and the
"Show subtitles" switch) to strings_<lang>.json, in the game's register per language (French / Spanish infinitive,
German / Italian imperative, Japanese です/ます within 20 characters a line). Then run check_strings.py and
emit_menu_texts_inc.py.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

ROWS = {
    'en': {
        'audio.music_volume.label': "Music volume",
        'audio.music_volume.help': "Adjust the volume of the music.",
        'audio.sfx_volume.label': "Sound effects volume",
        'audio.sfx_volume.help': "Adjust the volume of the sound effects.",
        'audio.voice_volume.label': "Voice volume",
        'audio.voice_volume.help': "Adjust the volume of the characters' voices.",
        'audio.subtitles_shown.label': "Show subtitles",
        'audio.subtitles_shown.help': "Show or hide the subtitles of the spoken\nlines.",
        'word.restart': "(restart)",
    },
    'fr': {
        'word.restart': "(redémarrage)",
        'audio.music_volume.label': "Volume musique",
        'audio.music_volume.help': "Régler le volume de la musique.",
        'audio.sfx_volume.label': "Volume des effets",
        'audio.sfx_volume.help': "Régler le volume des effets sonores.",
        'audio.voice_volume.label': "Volume des voix",
        'audio.voice_volume.help': "Régler le volume des voix des\npersonnages.",
        'audio.subtitles_shown.label': "Afficher sous-titres",
        'audio.subtitles_shown.help': "Afficher ou masquer les sous-titres des\ndialogues.",
    },
    'ge': {
        'word.restart': "(Neustart)",
        'audio.music_volume.label': "Musiklautstärke",
        'audio.music_volume.help': "Ändere die Lautstärke der Musik.",
        'audio.sfx_volume.label': "Effektlautstärke",
        'audio.sfx_volume.help': "Ändere die Lautstärke der Soundeffekte.",
        'audio.voice_volume.label': "Stimmenlautstärke",
        'audio.voice_volume.help': "Ändere die Lautstärke der Stimmen.",
        'audio.subtitles_shown.label': "Untertitel anzeigen",
        'audio.subtitles_shown.help': "Zeige oder verberge die Untertitel der\nDialoge.",
    },
    'it': {
        'word.restart': "(riavvio)",
        'audio.music_volume.label': "Volume musica",
        'audio.music_volume.help': "Regola il volume della musica.",
        'audio.sfx_volume.label': "Volume effetti",
        'audio.sfx_volume.help': "Regola il volume degli effetti sonori.",
        'audio.voice_volume.label': "Volume voci",
        'audio.voice_volume.help': "Regola il volume delle voci dei\npersonaggi.",
        'audio.subtitles_shown.label': "Mostra sottotitoli",
        'audio.subtitles_shown.help': "Mostra o nascondi i sottotitoli dei\ndialoghi.",
    },
    'sp': {
        'word.restart': "(reinicio)",
        'audio.music_volume.label': "Volumen de música",
        'audio.music_volume.help': "Ajustar el volumen de la música.",
        'audio.sfx_volume.label': "Volumen de efectos",
        'audio.sfx_volume.help': "Ajustar el volumen de los efectos de\nsonido.",
        'audio.voice_volume.label': "Volumen de voces",
        'audio.voice_volume.help': "Ajustar el volumen de las voces de los\npersonajes.",
        'audio.subtitles_shown.label': "Mostrar subtítulos",
        'audio.subtitles_shown.help': "Mostrar u ocultar los subtítulos de los\ndiálogos.",
    },
    'jp': {
        'word.restart': "（再起動）",
        'audio.music_volume.label': "音楽の音量",
        'audio.music_volume.help': "音楽の音量を調整します。",
        'audio.sfx_volume.label': "SEの音量",  # the JP sheets lack 効 (効果音): SE, the usual Japanese game term
        'audio.sfx_volume.help': "SEの音量を調整します。",
        'audio.voice_volume.label': "音声の音量",
        'audio.voice_volume.help': "キャラクターの音声の音量を調整します。",
        'audio.subtitles_shown.label': "字幕表示",
        'audio.subtitles_shown.help': "会話の字幕の表示を切り替えます。",
    },
}


def main():
    for lang, rows in ROWS.items():
        path = os.path.join(HERE, 'strings_%s.json' % lang)
        data = json.load(open(path, encoding='utf-8'))
        data.update(rows)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write('\n')
        print('%s: %d rows' % (lang, len(rows)))


if __name__ == '__main__':
    main()
