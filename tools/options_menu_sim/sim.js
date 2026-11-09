// Options menu simulator: a small CSD player for Options.gncp plus the menu logic.
// Data: generated/data.js (from the disc, via build_sim_data.py) and options.js (menu content).
'use strict';

const DATA = window.OPTIONS_DATA;
const MENU = window.OPTIONS_MENU;
const W = 640, H = 480;
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');

// ---------------------------------------------------------------------------------------------
// CSD player
//
// A cast's live state starts from its authored properties. A motion writes only the tracks it
// has; every other property keeps its current value, so poses chain (MenuOpen, then the pose of
// the selected icon). Keys: interp 1 = linear, 0 = step. Before the first key the first value
// holds, after the last the last value holds; the scene clamps to the motion's [start, end].

function makeScene(src) {
  const scene = { src, casts: [], byName: {}, motion: null, frame: 0, loop: false, onEnd: null };
  for (const c of src.casts) {
    const cast = { src: c, live: null };
    scene.casts.push(cast);
    scene.byName[c.name] = cast;
  }
  // pair = (first child, next sibling) within the group; a child is placed relative to its parent.
  for (const c of scene.casts) {
    const child = c.src.pair[0];
    if (child >= 0) {
      const k = scene.casts.find(o => o.src.group === c.src.group && o.src.index === child);
      if (k) k.parent = c;
    }
  }
  resetScene(scene);
  return scene;
}

function resetScene(scene) {
  for (const c of scene.casts) {
    const s = c.src;
    c.live = { x: s.x, y: s.y, rotation: s.rotation, scale_x: s.scale_x, scale_y: s.scale_y,
               pattern: s.pattern, colour: s.colour >>> 0, hide: s.hide };
  }
  scene.motion = null;
}

function evalTrack(keys, frame, isColour) {
  if (frame <= keys[0][0]) return keys[0][1];
  const last = keys[keys.length - 1];
  if (frame >= last[0]) return last[1];
  let i = 0;
  while (keys[i + 1][0] < frame) i++;
  const a = keys[i], b = keys[i + 1];
  if (a[2] === 0) return a[1];
  const t = (frame - a[0]) / (b[0] - a[0]);
  if (!isColour) return a[1] + (b[1] - a[1]) * t;
  let out = 0;
  for (let sh = 24; sh >= 0; sh -= 8) {
    const ca = (a[1] >>> sh) & 255, cb = (b[1] >>> sh) & 255;
    out = out * 256 + Math.round(ca + (cb - ca) * t);
  }
  return out >>> 0;
}

function applyMotion(scene) {
  const m = scene.motion;
  if (!m) return;
  m.groups.forEach((group, g) => group.forEach((tracks, i) => {
    const cast = scene.casts.find(c => c.src.group === g && c.src.index === i);
    if (!cast) return;
    for (const [name, keys] of Object.entries(tracks)) {
      if (!keys.length) continue;
      const v = evalTrack(keys, scene.frame, name === 'colour');
      if (name === 'cell') cast.live.pattern = v;
      else cast.live[name] = v;
    }
  }));
}

function play(scene, name, opts = {}) {
  const m = scene.src.motions.find(x => x.name === name);
  if (!m) throw new Error('no motion ' + name + ' in scene ' + scene.src.name);
  scene.motion = m;
  scene.frame = m.start;
  scene.loop = !!opts.loop;
  scene.onEnd = opts.onEnd || null;
  applyMotion(scene);
}

function stepScene(scene) {
  const m = scene.motion;
  if (!m) return;
  if (scene.frame >= m.end) {
    if (scene.loop) scene.frame = m.start;
    else {
      const cb = scene.onEnd;
      scene.onEnd = null;
      scene.motion = null;
      if (cb) cb();
      return;
    }
  } else {
    scene.frame = Math.min(scene.frame + 1, m.end);
  }
  applyMotion(scene);
}

// ---------------------------------------------------------------------------------------------
// Drawing

const images = {};
function image(name) {
  if (!images[name]) {
    const img = new Image();
    img.src = 'generated/tex/' + name + '.png';
    images[name] = img;
  }
  return images[name];
}

function castPlacement(cast) {
  let x = cast.live.x * W, y = cast.live.y * H, alpha = (cast.live.colour & 255) / 255;
  for (let p = cast.parent; p; p = p.parent) {
    x += p.live.x * W;
    y += p.live.y * H;
    alpha *= (p.live.colour & 255) / 255;
  }
  return { x, y, alpha };
}

// Quad corners in authored index order: 0 TL, 1 BL, 2 TR, 3 BR (see gncp_read.py).
function castCorners(cast) {
  const { x, y } = castPlacement(cast);
  const L = cast.live, rad = L.rotation * Math.PI / 180, cs = Math.cos(rad), sn = Math.sin(rad);
  return cast.src.local_corners.map(([lx, ly]) => {
    const px = lx * L.scale_x * W, py = ly * L.scale_y * H;
    return [px * cs + py * sn + x, -px * sn + py * cs + y];
  });
}

function castCell(scene, cast) {
  const n = Math.trunc(cast.live.pattern) % 10000;
  const pats = cast.src.patterns;
  if (n < 0 || n >= pats.length) return -1;
  return pats[n] >= 0 ? pats[n] : -1;
}

function drawScene(scene) {
  for (const cast of scene.casts) {
    if (cast.live.hide || cast.src.kind !== 1) continue;
    const override = iconOverride(scene, cast);
    if (override === 'skip') continue;
    if (override) { override(); continue; }
    const cellIndex = castCell(scene, cast);
    if (cellIndex < 0) continue;
    const { alpha } = castPlacement(cast);
    if (alpha <= 0) continue;
    const cell = scene.src.cells[cellIndex];
    const img = image('Options_' + state.lang + '__' + cell.tex);
    if (!img.complete || !img.naturalWidth) continue;
    const iw = img.naturalWidth, ih = img.naturalHeight;
    const sx = cell.u0 * iw, sy = cell.v0 * ih;
    const sw = Math.min((cell.u1 - cell.u0) * iw, iw - sx), sh = Math.min((cell.v1 - cell.v0) * ih, ih - sy);
    const p = castCorners(cast);
    ctx.save();
    ctx.globalAlpha = alpha;
    ctx.setTransform(p[2][0] - p[0][0], p[2][1] - p[0][1], p[1][0] - p[0][0], p[1][1] - p[0][1], p[0][0], p[0][1]);
    ctx.drawImage(img, sx, sy, sw, sh, 0, 0, 1, 1);
    ctx.restore();
  }
}

// Grid icons: casts of hidden categories are not drawn. An extra category draws its own tile in its borrowed
// slot: art = { normal, selected } names two images in generated/tex (see make_display_icon.py), or
// 'placeholder' draws a labelled stand-in.
function iconOverride(scene, cast) {
  if (scene !== scenes.menu) return null;
  const cats = categories();
  const i = cats.findIndex(c => c.cast === cast.src.name);
  if (i < 0) return MENU.categories.some(c => c.cast === cast.src.name) ? 'skip' : null;
  const art = cats[i].art;
  if (art && typeof art === 'object') return () => drawArtIcon(cast, art, i === state.cat);
  if (art !== 'placeholder') return null;
  return () => drawPlaceholderIcon(cast, cats[i], i === state.cat);
}

function drawArtIcon(cast, art, selected) {
  const { alpha } = castPlacement(cast);
  const img = image(selected ? art.selected : art.normal);
  if (alpha <= 0 || !img.complete || !img.naturalWidth) return;
  const p = castCorners(cast);
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.setTransform(p[2][0] - p[0][0], p[2][1] - p[0][1], p[1][0] - p[0][0], p[1][1] - p[0][1], p[0][0], p[0][1]);
  ctx.drawImage(img, 0, 0, 1, 1);
  ctx.restore();
}

function drawPlaceholderIcon(cast, cat, selected) {
  const { alpha } = castPlacement(cast);
  if (alpha <= 0) return;
  const p = castCorners(cast);
  const x0 = Math.min(p[0][0], p[3][0]), y0 = Math.min(p[0][1], p[3][1]);
  const w = Math.abs(p[3][0] - p[0][0]), h = Math.abs(p[3][1] - p[0][1]);
  ctx.save();
  ctx.globalAlpha = alpha;
  const g = ctx.createLinearGradient(x0, y0, x0 + w, y0 + h);
  g.addColorStop(0, '#b8b2aa');
  g.addColorStop(1, '#7c766f');
  ctx.fillStyle = g;
  ctx.fillRect(x0 + 2, y0 + 2, w - 4, h - 4);
  ctx.strokeStyle = '#4a4642';
  ctx.lineWidth = 2;
  ctx.strokeRect(x0 + 3, y0 + 3, w - 6, h - 6);
  ctx.fillStyle = '#5a5550';
  for (const [rx, ry] of [[7, 7], [w - 7, 7], [7, h - 7], [w - 7, h - 7]]) {
    ctx.beginPath(); ctx.arc(x0 + rx, y0 + ry, 2, 0, Math.PI * 2); ctx.fill();
  }
  ctx.restore();
  drawText(cat.name || cat.id, x0 + w / 2, y0 + h / 2, { scale: 0.75, align: 'center', alpha,
                                                         colour: selected ? '#c01010' : '#2a2724' });
}

// Text with the game's Advertise glyph sheets. Latin sheets have fixed 12 px cells; glyphs are
// spaced by their ink width plus one pixel. Characters a sheet lacks fall back to the English sheet,
// then to a system font (new rows may use digits or letters the game never needed).
function glyphOf(ch) {
  // A sheet can list a character it has no ink for (EN ':'); treat that as missing.
  for (const g of [(DATA.fonts[state.lang] || {})[ch], DATA.fonts.EN[ch]]) {
    if (g && g[6] > g[5]) return g;
  }
  return null;
}

function textWidth(str, scale = 1) {
  let w = 0;
  for (const ch of str) {
    const g = glyphOf(ch);
    if (ch === ' ') w += 6;
    else if (!g && ch === '%' && glyphOf('/') && glyphOf('o')) w += textWidth('/') + textWidth('o') * 0.4 + 1;
    else if (!g) { ctx.font = 'bold 20px sans-serif'; w += ctx.measureText(ch).width; }
    else if (g[3] > 12) w += g[3];
    else w += (g[6] - g[5]) + 1;
  }
  return w * scale;
}

// The glyph sheets are white; a coloured copy of a sheet is made once per colour.
const tinted = {};
function sheetImage(sheet, colour) {
  const img = image(sheet);
  if (!colour || !img.complete || !img.naturalWidth) return img;
  const key = sheet + colour;
  if (!tinted[key]) {
    const c = document.createElement('canvas');
    c.width = img.naturalWidth;
    c.height = img.naturalHeight;
    const g = c.getContext('2d');
    g.drawImage(img, 0, 0);
    g.globalCompositeOperation = 'source-in';
    g.fillStyle = colour;
    g.fillRect(0, 0, c.width, c.height);
    tinted[key] = c;
  }
  return tinted[key];
}

function drawText(str, x, y, { scale = 1, alpha = 1, align = 'left', colour = null } = {}) {
  if (alpha <= 0) return;
  if (align === 'right') x -= textWidth(str, scale);
  else if (align === 'center') x -= textWidth(str, scale) / 2;
  ctx.save();
  ctx.globalAlpha = alpha;
  for (const ch of str) {
    const g = glyphOf(ch);
    if (ch === ' ') { x += 6 * scale; continue; }
    // The game's sheets have no '%': build it from their '/' and two half-size 'o's.
    if (!g && ch === '%' && glyphOf('/') && glyphOf('o')) {
      const slash = textWidth('/', scale), o = textWidth('o', scale * 0.5);
      drawText('o', x, y - 5 * scale, { scale: scale * 0.5, alpha, colour });
      drawText('/', x + o * 0.4, y, { scale, alpha, colour });
      drawText('o', x + o * 0.4 + slash - o * 0.6, y + 5 * scale, { scale: scale * 0.5, alpha, colour });
      x += o * 0.4 + slash + o * 0.4 + scale;
      continue;
    }
    if (!g) {
      ctx.font = 'bold ' + Math.round(20 * scale) + 'px sans-serif';
      ctx.fillStyle = colour || '#fff';
      ctx.textBaseline = 'middle';
      ctx.fillText(ch, x, y);
      x += ctx.measureText(ch).width;
      continue;
    }
    const [sheet, gx, gy, gw, gh, inkL, inkR] = g;
    const img = sheetImage(sheet, colour);
    const cjk = gw > 12;
    const sx = cjk ? gx : gx + inkL, sw = cjk ? gw : Math.max(1, inkR - inkL);
    if (img.width) {
      ctx.drawImage(img, sx, gy, sw, gh, x, y - gh * scale / 2, sw * scale, gh * scale);
    }
    x += (cjk ? gw : sw + 1) * scale;
  }
  ctx.restore();
}

// ---------------------------------------------------------------------------------------------
// Menu state

const scenes = {};
for (const s of DATA.scenes) scenes[s.name] = makeScene(s);

const state = {
  lang: 'EN',
  phase: 'opening',          // opening | grid | panel | closing | closed
  cat: 0,                    // selected category
  row: 0,                    // panel cursor (value index for one-row categories, row index otherwise)
  scroll: 0,
  settings: {},
  toast: null,
  track: 0,                  // last sound test track picked
  extras: location.hash === '#extras',
};

function str(key) {
  const t = (DATA.strings[state.lang] || {})[key] ?? DATA.strings.EN[key];
  return (t ?? String(key)).replace(/\s*\n\s*/g, ' ');
}

function categories() {
  return MENU.categories.filter(c => !c.hidden)
    .concat(state.extras ? (MENU.extraCategories || []) : []);
}

// A row's values are a list, or a function of the current settings (a resolution list that follows the
// chosen ratio). `value` is the default index, or a function returning it.
function rowValues(r) {
  return typeof r.values === 'function' ? r.values(state.settings) : r.values;
}

function allRows() {
  return MENU.categories.concat(MENU.extraCategories || []).flatMap(c => c.rows).filter(r => !r.action);
}

function initSettings() {
  for (const r of allRows()) {
    if (!(r.id in state.settings)) state.settings[r.id] = typeof r.value === 'function' ? r.value(state.settings) : r.value || 0;
  }
  normalizeSettings();
}

// After a change, a dependent list can be shorter: keep every index inside its list.
function normalizeSettings() {
  for (const r of allRows()) {
    const n = rowValues(r).length;
    if (state.settings[r.id] >= n) state.settings[r.id] = n - 1;
  }
}

// What the settings mean on the game's command line (each row's `cli`), shown in the sidebar.
function commandLine() {
  const args = [];
  for (const r of allRows()) {
    if (r.cli) {
      const a = r.cli(rowValues(r)[state.settings[r.id]], state.settings);
      if (a) args.push(a);
    }
  }
  return args.join(' ');
}

// The layout has 8 text slots, but the reference shows 7 rows: slot 8 sits on the "select" button.
const TEXT_SLOTS = 7;

function open() {
  for (const s of Object.values(scenes)) resetScene(s);
  state.phase = 'opening';
  play(scenes.Loop, 'Open', { onEnd: () => play(scenes.Loop, 'Loop', { loop: true }) });
  play(scenes.backgound, 'BGOpen');
  play(scenes.fontnull, 'Open');
  play(scenes.menu, 'MenuOpen', { onEnd: () => { state.phase = 'grid'; selectCategory(state.cat); updateSidebar(); } });
}

function close() {
  state.phase = 'closing';
  play(scenes.menu, 'MenuClose');
  play(scenes.backgound, 'BGClose');
  play(scenes.fontnull, 'Close', { onEnd: () => { state.phase = 'closed'; updateSidebar(); } });
}

function selectCategory(i) {
  state.cat = i;
  play(scenes.menu, categories()[i].motion);
}

function enterPanel() {
  const c = categories()[state.cat];
  state.phase = 'panel';
  state.row = c.rows.length === 1 && !c.rows[0].action ? state.settings[c.rows[0].id] : 0;
  state.scroll = 0;
  play(scenes.fontnull, 'Select');
}

function moveCursorInPanel(d) {
  const c = categories()[state.cat];
  const count = c.rows.length === 1 ? rowValues(c.rows[0]).length : c.rows.length;
  state.row = (state.row + d + count) % count;
  play(scenes.fontnull, 'Select');
}

function toast(text) { state.toast = { text, t: 120 }; }

function confirmInPanel() {
  const c = categories()[state.cat];
  if (c.rows.length > 1) {
    const r = c.rows[state.row];
    if (r.action) runAction(r, 0);
    return;
  }
  const r = c.rows[0];
  if (r.action) { runAction(r, state.row); return; }
  state.settings[r.id] = state.row;
  state.phase = 'grid';
}

function runAction(row, index) {
  if (row.action === 'play') state.track = index;
  if (row.action === 'play') toast('♪ ' + str(rowValues(row)[index]) + '  (no audio in the simulator)');
  if (row.action === 'load') toast(str('load') + '  (simulated)');
}

function changeRowValue(d) {
  const c = categories()[state.cat];
  if (c.rows.length < 2) return;
  const r = c.rows[state.row];
  if (r.action) return;
  const n = rowValues(r).length;
  state.settings[r.id] = (state.settings[r.id] + d + n) % n;
  normalizeSettings();
}

// Grid navigation by position: the nearest icon in the pressed direction.
function iconCentre(cat) {
  const p = castCorners(scenes.menu.byName[cat.cast]);
  return [(p[0][0] + p[3][0]) / 2, (p[0][1] + p[3][1]) / 2];
}

function moveInGrid(dx, dy) {
  const cats = categories();
  const [cx, cy] = iconCentre(cats[state.cat]);
  let best = -1, bestScore = Infinity;
  cats.forEach((c, i) => {
    if (i === state.cat) return;
    const [x, y] = iconCentre(c);
    const along = (x - cx) * dx + (y - cy) * dy;
    if (along <= 1) return;
    const across = Math.abs((x - cx) * dy) + Math.abs((y - cy) * dx);
    const score = along + across * 3;
    if (score < bestScore) { bestScore = score; best = i; }
  });
  if (best >= 0) selectCategory(best);
}

function input(button) {
  if (state.phase === 'closed') { if (button === 'A') open(); return; }
  if (state.phase === 'grid') {
    if (button === 'up') moveInGrid(0, -1);
    else if (button === 'down') moveInGrid(0, 1);
    else if (button === 'left') moveInGrid(-1, 0);
    else if (button === 'right') moveInGrid(1, 0);
    else if (button === 'A') enterPanel();
    else if (button === 'B' || button === 'Y') close();
  } else if (state.phase === 'panel') {
    if (button === 'up') moveCursorInPanel(-1);
    else if (button === 'down') moveCursorInPanel(1);
    else if (button === 'left') changeRowValue(-1);
    else if (button === 'right') changeRowValue(1);
    else if (button === 'A') confirmInPanel();
    else if (button === 'B' || button === 'Y') state.phase = 'grid';
  }
  updateSidebar();
}

// ---------------------------------------------------------------------------------------------
// Panel and help text

const YELLOW = '#f0e848';

function drawPanel() {
  if (state.phase === 'closed') return;
  const bg = scenes.backgound.byName.menu_bg.live;
  if (bg.x > 0.60) return;                     // panel still sliding in or already gone
  const cats = categories();
  const c = cats[state.cat];
  const single = c.rows.length === 1;
  const lines = single
    ? rowValues(c.rows[0]).map(v => ({ left: str(v) }))
    : c.rows.map(r => ({ left: str(r.label || r.id), right: r.action ? '' : str(rowValues(r)[state.settings[r.id]]) }));
  const r0 = c.rows[0];
  const current = !single ? -1 : r0.action === 'play' ? state.track : r0.action ? -1 : state.settings[r0.id];
  const cursor = state.phase === 'panel' ? state.row : current;

  if (cursor - state.scroll >= TEXT_SLOTS) state.scroll = cursor - TEXT_SLOTS + 1;
  if (cursor >= 0 && cursor < state.scroll) state.scroll = cursor;

  const slots = [];
  for (let i = 1; i <= TEXT_SLOTS; i++) slots.push(castPlacement(scenes.fontnull.byName['text' + i]));

  // Highlight bar: the fontnull "highlight" cast, moved to the cursor row (the game moves it in code).
  const hl = scenes.fontnull.byName.highlight;
  // The reference (grid focus) shows no bar, only the current value in yellow; the bar is shown
  // while the cursor is in the panel (inferred).
  if (state.phase === 'panel' && cursor - state.scroll < TEXT_SLOTS) {
    const slot = slots[cursor - state.scroll];
    hl.live.x = (slot.x - 22) / W;
    hl.live.y = (slot.y - 5) / H;
    hl.live.hide = 0;
  } else {
    hl.live.hide = 1;
  }
  drawScene(scenes.fontnull);

  lines.slice(state.scroll, state.scroll + TEXT_SLOTS).forEach((ln, i) => {
    const slot = slots[i];
    // Text origin measured on the reference: 3 px right of and 5 px above the slot centre.
    const x = slot.x + 3, y = slot.y - 5;
    const index = state.scroll + i;
    const focused = state.phase === 'panel' && index === state.row;
    const colour = index === current ? YELLOW : null;
    const v = ln.right != null && ln.right !== '' ? (focused && !single ? '< ' + ln.right + ' >' : ln.right) : '';
    // A label and value wider than the panel are drawn smaller to fit (a 12 px gap between them).
    let scale = single ? 1 : 0.85;
    if (v) {
      const need = textWidth(ln.left, scale) + textWidth(v, scale) + 12;
      if (need > 622 - x) scale *= (622 - x) / need;
    }
    drawText(ln.left, x, y, { scale, colour });
    if (v) drawText(v, 622, y, { scale, align: 'right' });
  });
}

function drawHelp() {
  const nb = scenes.menu.byName.Null_box;
  const { x, y } = castPlacement(nb);
  const reveal = Math.max(0, Math.min(1, nb.live.scale_x));
  if (reveal <= 0 || state.phase === 'closed') return;
  const c = categories()[state.cat];
  const raw = (DATA.strings[state.lang] || {})[c.help] ?? DATA.strings.EN[c.help] ?? c.help;
  ctx.save();
  ctx.beginPath();
  ctx.rect(x - 8, y - 30, 480 * reveal, 70);
  ctx.clip();
  raw.split('\n').forEach((line, i) => drawText(line.trim(), x - 1, y - 5 + i * 22, { scale: 1 }));
  ctx.restore();
}

function drawOverlay() {
  if (state.phase === 'closed') {
    drawText(str('main_menu'), W / 2, H / 2 - 16, { align: 'center' });
    drawText('A: ' + str('options'), W / 2, H / 2 + 16, { scale: 0.8, align: 'center' });
  }
  if (state.toast) {
    ctx.fillStyle = 'rgba(0,0,0,0.75)';
    ctx.fillRect(40, 440, 560, 32);
    drawText(state.toast.text, W / 2, 456, { scale: 0.8, align: 'center' });
    if (--state.toast.t <= 0) state.toast = null;
  }
}

function render() {
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.globalAlpha = 1;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W, H);
  if (state.phase !== 'closed') {          // closed = the game has gone back to the main menu
    drawScene(scenes.Loop);
    drawScene(scenes.backgound);
    drawScene(scenes.menu);
    drawPanel();
    drawHelp();
  }
  drawOverlay();
}

// ---------------------------------------------------------------------------------------------
// Loop, input devices, sidebar

let last = performance.now(), acc = 0;
function tick(now) {
  acc += Math.min(now - last, 250);
  last = now;
  pollGamepad();
  while (acc >= 1000 / 60) {
    for (const s of Object.values(scenes)) stepScene(s);
    acc -= 1000 / 60;
  }
  render();
  requestAnimationFrame(tick);
}

const KEYS = {
  ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
  Enter: 'A', Space: 'A', KeyZ: 'A', Escape: 'B', Backspace: 'B', KeyX: 'B', KeyC: 'Y',
};
window.addEventListener('keydown', e => {
  const b = KEYS[e.code];
  if (!b || e.target.tagName === 'SELECT') return;
  e.preventDefault();
  input(b);
});

// Read-only gamepad polling (standard mapping); the page never sends rumble.
const padHeld = {};
function pollGamepad() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  const pad = Array.from(pads).find(p => p);
  if (!pad) return;
  const ax = pad.axes[0] || 0, ay = pad.axes[1] || 0;
  const now = performance.now();
  const map = {
    A: pad.buttons[0]?.pressed, B: pad.buttons[1]?.pressed, Y: pad.buttons[3]?.pressed,
    up: pad.buttons[12]?.pressed || ay < -0.6, down: pad.buttons[13]?.pressed || ay > 0.6,
    left: pad.buttons[14]?.pressed || ax < -0.6, right: pad.buttons[15]?.pressed || ax > 0.6,
  };
  for (const [b, down] of Object.entries(map)) {
    const h = padHeld[b];
    if (down && !h) { padHeld[b] = now + 400; input(b); }
    else if (down && h && now > h && b.length > 1) { padHeld[b] = now + 120; input(b); }
    else if (!down) padHeld[b] = 0;
  }
}

function updateSidebar() {
  const out = document.getElementById('settings');
  const lines = [];
  for (const c of categories()) {
    for (const r of c.rows) {
      if (r.action) continue;
      lines.push(r.id + ' = ' + str(rowValues(r)[state.settings[r.id]]));
    }
  }
  const cli = state.extras ? '\n\ngame command line:\n' + commandLine() : '';
  out.textContent = 'phase: ' + state.phase + '\ncategory: ' + categories()[state.cat].id + '\n\n' + lines.join('\n') + cli;
}

const langSel = document.getElementById('lang');
for (const l of DATA.langs) langSel.add(new Option(l, l));
langSel.value = state.lang;
langSel.addEventListener('change', () => { state.lang = langSel.value; updateSidebar(); langSel.blur(); });

const extrasBox = document.getElementById('extras');
extrasBox.checked = state.extras;
extrasBox.addEventListener('change', () => {
  state.extras = extrasBox.checked;
  if (state.cat >= categories().length) {
    state.cat = 0;
    if (state.phase === 'panel') state.phase = 'grid';
    if (state.phase === 'grid') selectCategory(0);
  }
  updateSidebar();
  extrasBox.blur();
});

document.getElementById('replay').addEventListener('click', e => { open(); e.target.blur(); });

initSettings();
open();
updateSidebar();
requestAnimationFrame(tick);
// step(n): advance n frames and redraw without the animation loop (tests, hidden tabs).
function step(n = 1) {
  for (let i = 0; i < n; i++) for (const s of Object.values(scenes)) stepScene(s);
  render();
  return state.phase;
}
window.OPTIONS_SIM = { state, scenes, input, open, close, step };
