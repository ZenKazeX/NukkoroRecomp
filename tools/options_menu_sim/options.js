// What the Options menu contains. Edit this file to add settings; the simulator needs no other change.
//
// A category is one icon in the 2x3 grid. `cast` is its icon cast in Options.gncp (scene "menu")
// and `motion` the pose the game plays while that icon is selected (it lights the icon).
// `help` is the line shown at the top; a key of the front-end message table or literal text.
//
// A row is one setting:
//   id      key in the settings object
//   label   shown only when the category has more than one row (string key or literal text)
//   values  list of string keys (from Advertise_<lang>.fnt, see build_sim_data.py STRING_IDS)
//           or literal text; anything that is not a key is drawn as written
//   value   index of the default
//   action  optional: 'play' or 'load' - the row is a command, not a stored setting
//
// A category with ONE row behaves like the game: the right panel lists the values and the
// current one is drawn yellow. With SEVERAL rows the panel lists "label  value" and left/right
// change it.
//
// Ground truth for the layout: source/.build/validation/menu-ref/dolphin-options-sound-test.png
// (retail PAL in Dolphin). The PAL GameCube screen shows FIVE tiles: the voice-language tile
// (icn_audio, bottom right) is authored in the layout but not shown, so it is `hidden` here.
// The order of the subtitle language list is inferred, not checked against the running game.

const TRACKS = Array.from({ length: 55 }, (_, i) => 'track_' + (611 + i));   // I AM .. ENDING

// The launcher's resolution presets and ratio families (tools/resolution_presets.py PRESETS / FAMILIES).
const RATIOS = ['Auto', '4:3', '5:4', '16:9', '16:10', '21:9', '32:10', '32:9'];
const PRESETS = [
  [1280, 720, '16:9'], [1366, 768, '16:9'], [1600, 900, '16:9'], [1920, 1080, '16:9'], [2560, 1440, '16:9'],
  [3200, 1800, '16:9'], [3840, 2160, '16:9'], [5120, 2880, '16:9'], [7680, 4320, '16:9'],
  [1280, 800, '16:10'], [1440, 900, '16:10'], [1680, 1050, '16:10'], [1920, 1200, '16:10'],
  [2560, 1600, '16:10'], [2880, 1800, '16:10'], [3840, 2400, '16:10'], [5120, 3200, '16:10'],
  [2560, 1080, '21:9'], [3440, 1440, '21:9'], [3840, 1600, '21:9'], [5120, 2160, '21:9'],
  [3840, 1200, '32:10'], [5120, 1600, '32:10'], [3840, 1080, '32:9'], [5120, 1440, '32:9'], [7680, 2160, '32:9'],
  [640, 480, '4:3'], [800, 600, '4:3'], [1024, 768, '4:3'], [1280, 960, '4:3'], [1600, 1200, '4:3'],
  [1280, 1024, '5:4'],
];

function resolutions(ratio) {
  return PRESETS.filter(p => ratio === 'Auto' || p[2] === ratio)
    .sort((a, b) => a[0] * a[1] - b[0] * b[1])
    .map(p => p[0] + ' x ' + p[1]);
}

// Render resolution as a percentage of the screen resolution (per side). The engine takes an exact size, so a
// percentage becomes that many pixels of the picture, rounded; sizes past 16384 per side (the engine's limit)
// are not offered. Above 100 % the picture is supersampled, below it is scaled up.
const RENDER_PERCENTS = [50, 67, 75, 100, 125, 150, 200, 250, 300, 400];

function renderSizes(picture) {
  const [w, h] = picture.split(' x ').map(Number);
  return RENDER_PERCENTS.filter(p => Math.round(w * p / 100) <= 16384 && Math.round(h * p / 100) <= 16384)
    .map(p => p + '%');
}

function renderPixels(picture, percent) {
  const [w, h] = picture.split(' x ').map(Number);
  const p = parseInt(percent, 10);
  return [Math.max(1, Math.round(w * p / 100)), Math.max(1, Math.round(h * p / 100))];
}

window.OPTIONS_MENU = {
  categories: [
    { id: 'sound', cast: 'icon_sound', motion: 'Sound', help: 'help_soundtest',
      rows: [{ id: 'soundtest', action: 'play', values: TRACKS }] },
    { id: 'system', cast: 'icn_system', motion: 'System', help: 'help_speaker',
      rows: [{ id: 'speaker', values: ['stereo', 'mono', 'dolby_pl2'], value: 0 }] },
    { id: 'load', cast: 'icn_load', motion: 'Load', help: 'help_load',
      rows: [{ id: 'load', action: 'load', values: ['load'] }] },
    { id: 'vibe', cast: 'icn_vibe', motion: 'Vibe', help: 'help_vibration',
      rows: [{ id: 'vibration', values: ['on', 'off'], value: 0 }] },
    { id: 'subtitle', cast: 'icn_subtitle', motion: 'Subtitle', help: 'help_subtitle',
      rows: [{ id: 'subtitles', values: ['lang_en', 'lang_fr', 'lang_ge', 'lang_it', 'lang_sp', 'lang_jp'], value: 0 }] },
    { id: 'audio', cast: 'icn_audio', motion: 'Audio', help: 'help_voice', hidden: true,
      rows: [{ id: 'voice', values: ['lang_en', 'lang_jp'], value: 0 }] },
  ],

  // Categories that are not in the game, shown when "PC settings" is ticked (or the URL ends in
  // #extras). They take a grid slot the PAL screen leaves empty: `cast`/`motion` borrow that
  // slot's position and pose. `art` names the tile images (normal / selected) in generated/tex;
  // `art: 'placeholder'` draws a stand-in tile labelled `name` instead.
  extraCategories: [
    { id: 'display', cast: 'icn_audio', motion: 'Audio', name: 'DISPLAY',
      art: { normal: 'icon_display', selected: 'icon_display_hi' },   // make_display_icon.py
      help: 'pc_help',
      // Every row is a setting the port accepts today (2026-10-28); `cli` is the game's own flag for it.
      // Sources: runtime/src/shadow_main.cpp (flags), tools/resolution_presets.py (presets and ratio
      // families, mirrored from lift/render/display_modes.cpp), docs/RESOLUTION_AUDIT.md (render size).
      rows: [
        { id: 'display_mode', label: 'pc_display_mode', values: ['pc_windowed', 'pc_fullscreen'], value: 0,
          cli: v => v === 'pc_fullscreen' ? '--fullscreen' : '--windowed' },
        // The ratio only filters the resolution list (as the launcher does); the game takes its aspect
        // from the resolution itself.
        { id: 'ratio', label: 'pc_screen_ratio', values: ['pc_auto'].concat(RATIOS.slice(1)), value: 0 },
        { id: 'resolution', label: 'pc_resolution', values: s => resolutions(RATIOS[s.ratio]),
          value: s => Math.max(0, resolutions(RATIOS[s.ratio]).indexOf('1920 x 1080')),
          cli: v => { const [w, h] = v.split(' x '); return '--width ' + w + ' --height ' + h; } },
        // A percentage of the resolution; the engine renders at exactly the resulting size and fits it to
        // the screen (supersampled above 100 %).
        { id: 'render', label: 'pc_render_resolution',
          values: s => renderSizes(resolutions(RATIOS[s.ratio])[s.resolution]),
          value: s => renderSizes(resolutions(RATIOS[s.ratio])[s.resolution]).indexOf('100%'),
          cli: (v, s) => {
            const [w, h] = renderPixels(resolutions(RATIOS[s.ratio])[s.resolution], v);
            return '--render-width ' + w + ' --render-height ' + h;
          } },
        { id: 'fps', label: 'pc_frame_rate', values: ['30', '60', '120', '144', '165', '240', 'pc_unlocked'], value: 1,
          cli: v => '--render-rate ' + (v === 'pc_unlocked' ? 'unlocked' : v) },
        { id: 'vsync', label: 'pc_vsync', values: ['on', 'off'], value: 0,
          cli: v => v === 'on' ? '--vsync' : '--no-vsync' },
        { id: 'blend', label: 'pc_frame_blending', values: ['off', 'on'], value: 0,
          cli: v => v === 'on' ? '--frame-interpolation' : '' },
        { id: 'draw', label: 'pc_draw_distance', value: 7,
          values: ['pc_draw_1', 'x2', 'x3', 'x4', 'x5', 'x6', 'x7', 'pc_draw_8'],
          cli: (v, s) => '--draw-distance ' + (s.draw + 1) },
        { id: 'fpsc', label: 'pc_fps_counter', values: ['off', 'on'], value: 0,
          cli: v => v === 'on' ? '--fps-counter' : '' },
      ] },
  ],
};
