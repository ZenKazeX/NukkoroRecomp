#!/usr/bin/env python3
"""The review of the first machine translations (2026-11-05), applied to strings_<lang>.json.

    python review_2026-11-05.py      # then: python check_strings.py <LANG> for each, and emit_menu_texts_inc.py

What it fixes:
  * the game's own wording where the game already has the text, in every language (Advertise_<lang>.fnt messages 5001
    voice language, 5002 subtitles, 5003 sound test, 5004 speaker output, 5703 vibration, 5006 Load Game Data);
  * the game's register: German helps in the imperative ("Ändere ..."), Spanish in the infinitive ("Cambiar ..."), as
    the game's own help lines are; French infinitive and Italian imperative were already right;
  * German: broken sentences (render scale, Load Game Data, the Controls description), an English "Autosave", a colon
    the front-end sheets do not ink;
  * French: the HUD named "l'affichage", the confirmation's time-out lost;
  * Italian: "camera" for the game camera is "telecamera"; Italian / Spanish "B Return" means "undo", not "go back";
  * Japanese: lines of at most 20 characters (the game's own Japanese help lines run up to 20-21; its glyphs are twice
    as wide as the Latin ones), the game's spelling コントローラ, no "s" for seconds (the sheets lack 秒), clearer
    camera labels (the sheets lack 転 for 反転).
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))

FIXES = {
    'fr': {
        'audio.speaker.help': "Modifier les paramètres audio.",
        'controls.vibration.help': "Modifier les paramètres de vibration\nde la manette.",
        'gameplay.load_game.help': "Charger données. Progression et modi-\nfications sauvegardées automatiquement.",
        'graphics.render_scale.help': "Régler la résolution interne de la 3D,\nsans toucher à l'écran ni à l'interface.",
        'menu.confirm.help': "Garder les nouveaux réglages d'écran ?\nSinon, les anciens reviennent après 15 s.",
    },
    'ge': {
        'category.display.help': "Ändere Bildschirmmodus, Auflösung und\nBildformat.",
        'category.graphics.help': "Ändere die Darstellung des 3D-Bildes.",  # the GE sheet has no 'Q'
        'category.audio.help': "Ändere Lautstärke, Lautsprecherausgabe\nsowie Sprach- und Untertitelsprache.",
        'category.controls.help': "Ändere die Vibration des Controllers\nund den Kamera-Stick.",
        'category.savedata.help': "Lade Spieldaten und ändere die\nSpeichereinstellungen.",
        'category.system.help': "Setze die Standardwerte zurück und sieh\ndir die Versionsinformationen an.",
        'display.mode.help': "Fenster, Vollbild oder exklusiver Modus.\nBestätige Änderungen binnen 15 Sekunden.",
        'display.resolution.help': "Ändere die Bildschirmauflösung. Höhere\nAuflösungen zeigen ein schärferes Bild.",
        'display.aspect.help': "Wähle das Format. Auto füllt den Bildschirm,\nfeste Formate zeigen schwarze Balken.",
        'display.vsync.help': "Zeige Bilder synchron zur Bildwiederhol-\nrate an. Das verhindert Bildrisse.",
        'display.fps_counter.help': "Zeige die Bildrate in einer Ecke des\nBildschirms an.",
        'display.defaults.help': "Setze die Anzeigeeinstellungen auf die\nStandardwerte zurück.",
        'graphics.render_scale.help': "Ändere die interne 3D-Auflösung. Bild-\nschirmauflösung und HUD bleiben gleich.",
        'graphics.msaa.help': "Glätte die Treppeneffekte an 3D-Kanten\nmit MSAA (Multisampling).",
        'graphics.anisotropy.help': "Anisotrope Filterung hält Texturen auf\nsteil betrachteten Flächen scharf.",
        'graphics.defaults.help': "Setze die Grafikeinstellungen auf die\nStandardwerte zurück.",
        'audio.master_volume.help': "Ändere die Gesamtlautstärke des Spiels.",
        'audio.speaker.help': "Ändere die Optionen für\ndie Sound-Ausgabe.",
        'audio.voice.help': "Ändere die verwendete Sprache\nfür die Dialoge.",
        'audio.subtitles.help': "Ändere die Sprache der Untertitel und\naller Texte des Spiels.",
        'audio.sound_test.help': "Höre die Musik des Spiels an.",
        'menu.sound_test.help': "Höre die Musik des Spiels an.",
        'audio.defaults.help': "Setze Lautstärke und Lautsprecherausgabe\nauf die Standardwerte zurück.",
        'controls.vibration.help': "Ändere die Rumble-\neinstellung des Controllers.",
        'controls.rumble_strength.help': "Ändere, wie stark der Controller\nvibriert.",
        'controls.invert_camera_x.help': "Kehre die Links-Rechts-Richtung des\nKamera-Sticks um.",
        'controls.invert_camera_y.help': "Kehre die Oben-Unten-Richtung des\nKamera-Sticks um.",
        'controls.defaults.help': "Setze die Controller-Einstellungen auf\ndie Standardwerte zurück.",
        'gameplay.load_game.help': "Lade Spieldaten. Spielstand und\nÄnderungen werden autom. gespeichert.",
        'gameplay.autosave.label': "Autom. Speichern",
        'gameplay.autosave.help': "Schalte das automatische Speichern des\nSpiels ein oder aus.",
        'gameplay.defaults.help': "Setze die Speichereinstellungen auf die\nStandardwerte zurück.",
        'system.restore_all.help': "Setze alle Einstellungen dieses Menüs\nzurück, außer den Sprachen.",
        'menu.confirm.help': "Neue Anzeigeeinstellungen behalten? Sonst\ngelten nach Ablauf wieder die alten.",
        'word.confirm.question': "Einstellungen behalten?",
        'word.confirm.return': "B  Verwerfen",
    },
    'it': {
        'category.controls.help': "Cambia la vibrazione del controller e le\nimpostazioni della telecamera.",
        'audio.speaker.help': "Cambia l'uscita audio.",
        'controls.vibration.help': "Cambia i parametri di Rumble del\ncontroller.",
        'controls.invert_camera_x.label': "Inverti telecamera X",
        'controls.invert_camera_y.label': "Inverti telecamera Y",
        'controls.invert_camera_x.help': "Inverte la direzione sinistra-destra\ndella levetta della telecamera.",
        'controls.invert_camera_y.help': "Inverte la direzione alto-basso della\nlevetta della telecamera.",
        'gameplay.load_game.help': "Carica i dati. Progressi e modifiche\nsaranno salvati automaticamente.",
        'word.confirm.return': "B  Annulla",
    },
    'sp': {
        'category.display.help': "Cambiar el modo de pantalla, la resolución\ny la proporción de la imagen.",
        'category.graphics.help': "Cambiar la calidad de la imagen 3D.",
        'category.audio.help': "Ajustar el volumen, la salida de sonido y\nlos idiomas de voz y subtítulos.",
        'category.controls.help': "Ajustar la vibración del mando y los\najustes del stick de cámara.",
        'category.system.help': "Restaurar los valores predeterminados y\nconsultar la información de la versión.",
        'display.resolution.help': "Cambiar la resolución de la pantalla. Más\nresolución da una imagen más nítida.",
        'display.vsync.help': "Mostrar cada fotograma sincronizado con\nla pantalla para evitar cortes.",
        'display.fps_counter.help': "Mostrar la tasa de fotogramas en una\nesquina de la pantalla.",
        'display.defaults.help': "Devolver los ajustes de pantalla a sus\nvalores predeterminados.",
        'graphics.render_scale.help': "Ajustar la resolución 3D interna del\njuego. No cambia la pantalla ni el HUD.",
        'graphics.msaa.help': "Reducir los bordes dentados de los\nobjetos 3D mediante MSAA (multimuestreo).",
        'graphics.defaults.help': "Devolver los ajustes gráficos a sus\nvalores predeterminados.",
        'audio.master_volume.help': "Ajustar el volumen general del juego.",
        'audio.speaker.help': "Cambiar la configuración\nde los altavoces.",
        'audio.voice.help': "Cambiar el idioma de los personajes.",
        'audio.subtitles.help': "Cambiar el idioma de los subtítulos y\nde todo el texto del juego.",
        'audio.sound_test.help': "Escuchar la música del juego.",
        'menu.sound_test.help': "Escuchar la música del juego.",
        'audio.defaults.help': "Devolver el volumen y la salida de sonido\na sus valores predeterminados.",
        'controls.vibration.help': "Activar o desactivar la vibración del\nmando.",
        'controls.rumble_strength.help': "Ajustar la fuerza con que vibra el mando.",
        'controls.invert_camera_x.help': "Invertir la dirección izquierda-derecha\ndel stick de cámara.",
        'controls.invert_camera_y.help': "Invertir la dirección arriba-abajo del\nstick de cámara.",
        'controls.defaults.help': "Devolver los ajustes del mando a sus\nvalores predeterminados.",
        'gameplay.load_game.help': "Cargar datos de juego. Tu progreso\ny cambios se guardan automáticamente.",
        'gameplay.autosave.help': "Activar o desactivar el guardado\nautomático del juego.",
        'gameplay.defaults.help': "Devolver los ajustes de guardado a sus\nvalores predeterminados.",
        'system.restore_all.help': "Devolver todos los ajustes de este menú a\nsu valor por defecto, salvo los idiomas.",
        'word.confirm.return': "B  Deshacer",
    },
    'jp': {
        'category.display.help': "画面の表示方式、解像度、\n画面の形を変更します。",
        'category.audio.help': "音量、スピーカー出力、音声と\n字幕の言語を変更します。",
        'category.controls.help': "コントローラの振動と\nカメラスティックの設定を変更します。",
        'category.savedata.help': "ゲームデータをロードし、\nセーブ設定を変更します。",
        'display.mode.help': "ウィンドウ、全画面、エクスクルーシブを\n選びます。変更後は確定が必要です。",
        'display.aspect.help': "画面の形を選びます。自動は全画面で\n表示し、形を決めると黒い帯が入ります。",
        'graphics.msaa.help': "MSAAで3Dのギザギザを\n目立たなくします。",
        'display.vsync.help': "映像を画面の更新に合わせて表示し、\n映像のずれをなくします。",
        'graphics.render_scale.help': "画面の解像度やHUDは変えずに、\n3Dの解像度だけを調整します。",
        'graphics.anisotropy.label': "テクスチャフィルタ",
        'graphics.anisotropy.help': "急な角度で見る面のテクスチャを\n鮮明に保ちます。",
        'audio.voice.help': "音声で流れてくる言語を変更します。",
        'audio.subtitles.help': "字幕で表示される言語を変更します。",
        'audio.sound_test.help': "ゲーム中に登場したサウンドを確認します。",
        'menu.sound_test.help': "ゲーム中に登場したサウンドを確認します。",
        'audio.speaker.help': "サウンドの出力を切り替えます。",
        'audio.defaults.help': "音量とスピーカー出力を\n初期設定に戻します。",
        'controls.vibration.help': "コントローラの振動機能を切り替えます。",
        'controls.rumble_strength.help': "コントローラの振動の強さを調整します。",
        'controls.invert_camera_x.label': "カメラ左右反対",
        'controls.invert_camera_y.label': "カメラ上下反対",
        'controls.invert_camera_x.help': "カメラスティックの左右の\n操作を逆にします。",
        'controls.invert_camera_y.help': "カメラスティックの上下の\n操作を逆にします。",
        'controls.defaults.help': "コントローラ設定を初期設定に戻します。",
        'gameplay.load_game.label': "データロード",
        'gameplay.load_game.help': "ゲームデータをロードします。以降はこの\nゲームデータにオートセーブされます。",
        'gameplay.autosave.help': "オートセーブのオン・オフを\n切り替えます。",
        'menu.confirm.help': "新しい表示設定を保持しますか？\n時間切れで以前の設定に戻ります。",
        'word.confirm.returning': "あと %d で元に戻ります",
        'word.confirm.return': "B  元に戻す",
    },
}


def main():
    for lang, fixes in FIXES.items():
        path = os.path.join(HERE, 'strings_%s.json' % lang)
        data = json.load(open(path, encoding='utf-8'))
        for key, text in fixes.items():
            if key not in data:
                raise SystemExit('%s: no key %s' % (lang, key))
            data[key] = text
        if lang == 'jp':  # the game's own spelling
            data = {k: v.replace('コントローラー', 'コントローラ') for k, v in data.items()}
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write('\n')
        print('%s: %d fixes' % (lang, len(fixes)))


if __name__ == '__main__':
    main()
