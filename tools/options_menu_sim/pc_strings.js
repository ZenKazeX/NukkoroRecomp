// Texts of the PC settings (the Display category) in the game's six languages, merged into the game's own
// string tables (generated/data.js) under "pc_" keys. Translated 2026-10-28 by a Haiku subagent against the game's
// own Options texts for tone and wording; adjusted by hand: the IT and SP draw-distance terms, and three JP
// terms (aspect, internal resolution, distant-view display) because the JP glyph sheets lack 比 率 描 距 離 - every
// character here exists in its language's Advertise sheets. Not reviewed by a native speaker.
(function () {
  const T = {
    EN: { help: 'Change the screen and picture settings.', display_mode: 'Display mode', screen_ratio: 'Screen ratio',
          resolution: 'Resolution', render_resolution: 'Render resolution', frame_rate: 'Frame rate', vsync: 'V-Sync',
          frame_blending: 'Frame blending', draw_distance: 'Draw distance', fps_counter: 'FPS counter',
          windowed: 'Windowed', fullscreen: 'Fullscreen', auto: 'Auto', unlocked: 'Unlocked', original: 'Original',
          maximum: 'Maximum' },
    FR: { help: "Modifier les paramètres de l'écran et de l'image.", display_mode: "Mode d'affichage",
          screen_ratio: "Format d'écran", resolution: 'Résolution', render_resolution: 'Résolution de rendu',
          frame_rate: "Fréquence d'images", vsync: 'V-Sync', frame_blending: "Fusion d'images",
          draw_distance: "Distance d'affichage", fps_counter: 'Compteur FPS', windowed: 'Fenêtré',
          fullscreen: 'Plein écran', auto: 'Auto', unlocked: 'Illimité', original: 'Original', maximum: 'Maximum' },
    GE: { help: 'Ändere die Bildschirm- und Bildeinstellungen.', display_mode: 'Anzeigemodus', screen_ratio: 'Bildformat',
          resolution: 'Auflösung', render_resolution: 'Renderauflösung', frame_rate: 'Bildrate', vsync: 'V-Sync',
          frame_blending: 'Bildmischung', draw_distance: 'Sichtweite', fps_counter: 'FPS-Anzeige', windowed: 'Fenster',
          fullscreen: 'Vollbild', auto: 'Auto', unlocked: 'Unbegrenzt', original: 'Original', maximum: 'Maximum' },
    IT: { help: "Cambia i parametri dello schermo e dell'immagine.", display_mode: 'Modalità schermo',
          screen_ratio: 'Formato schermo', resolution: 'Risoluzione', render_resolution: 'Risoluzione render',
          frame_rate: 'Frequenza fotogrammi', vsync: 'V-Sync', frame_blending: 'Fusione fotogrammi',
          draw_distance: 'Distanza visiva', fps_counter: 'Contatore FPS', windowed: 'Finestra',
          fullscreen: 'Schermo intero', auto: 'Auto', unlocked: 'Sbloccato', original: 'Originale', maximum: 'Massimo' },
    JP: { help: '画面と映像の設定を変更します。', display_mode: '画面モード', screen_ratio: 'アスペクト', resolution: '解像度',
          render_resolution: '内部解像度', frame_rate: 'フレームレート', vsync: 'V-Sync', frame_blending: 'フレームブレンド',
          draw_distance: '遠景表示', fps_counter: 'FPS表示', windowed: 'ウィンドウ', fullscreen: 'フルスクリーン',
          auto: '自動', unlocked: '無制限', original: 'オリジナル', maximum: '最大' },
    SP: { help: 'Cambiar la configuración de la pantalla y la imagen.', display_mode: 'Modo de pantalla',
          screen_ratio: 'Proporción', resolution: 'Resolución', render_resolution: 'Resolución de render',
          frame_rate: 'Tasa de cuadros', vsync: 'V-Sync', frame_blending: 'Mezcla de cuadros',
          draw_distance: 'Distancia de visión', fps_counter: 'Contador FPS', windowed: 'Ventana',
          fullscreen: 'Pantalla completa', auto: 'Auto', unlocked: 'Desbloqueado', original: 'Original',
          maximum: 'Máximo' },
  };
  window.PC_STRINGS = T;
  const strings = window.OPTIONS_DATA.strings;
  for (const [lang, t] of Object.entries(T)) {
    const table = strings[lang] || (strings[lang] = {});
    for (const [k, v] of Object.entries(t)) table['pc_' + k] = v;
    table.pc_draw_1 = 'x1 ' + t.original;      // draw distance's ends carry a word
    table.pc_draw_8 = 'x8 ' + t.maximum;
  }
})();
