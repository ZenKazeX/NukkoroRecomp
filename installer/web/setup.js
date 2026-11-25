// Shadow the Hedgehog Recompiled setup page. Five steps: Welcome (the extracted disc), Japanese Voices (the optional
// Japanese disc), Install Location, Components (shortcuts + build tools), Install (review, then the real build and copy
// via the host).
'use strict';

// ---- native bridge ---------------------------------------------------------------------
const native = (() => {
  const view = window.chrome && window.chrome.webview;
  let nextId = 0;
  const pending = new Map();
  const listeners = [];
  if (view) view.addEventListener('message', ({ data }) => {
    if (data && Number.isInteger(data.id) && pending.has(data.id)) {
      const { resolve, reject } = pending.get(data.id);
      pending.delete(data.id);
      if (data.error) reject(new Error(data.error)); else resolve(data);
    } else if (data && data.type) listeners.forEach(fn => fn(data));
  });
  return {
    available: !!view,
    request(command, payload = '') {
      if (!view) return Promise.reject(new Error('This page must run inside the setup program.'));
      return new Promise((resolve, reject) => {
        const id = ++nextId;
        pending.set(id, { resolve, reject });
        view.postMessage(`${command}|${id}|${payload}`);
      });
    },
    send(message) { if (view) view.postMessage(message); },
    on(fn) { listeners.push(fn); },
    backend(...args) { return this.request('backend', args.join('\t')).then(reply => reply.result); },
  };
})();

// ---- state -------------------------------------------------------------------------------
const steps = ['Welcome', 'Voices', 'Install Location', 'Components', 'Install'];
const state = {
  step: 1,
  disc: { path: '', result: null, checking: false },
  // Optional: the Japanese disc, for the Japanese event and dialogue voices (copied to voice_jp).
  jp: { path: '', result: null, checking: false },
  speakJapanese: true,
  target: { path: '', result: null, checking: false },
  launch: true, desktop: true, startMenu: true,
  prereqs: null,
  phase: 'review',            // review | running | complete | failed | cancelled
  progress: 0, stageIndex: 0, stageName: '', log: '', error: '', logPath: '', cancelling: false,
};
const $ = selector => document.querySelector(selector);
const content = $('#content'), back = $('#back'), next = $('#next');
const escapeText = value => String(value ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const size = bytes => bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB` : `${Math.round(bytes / 1024 ** 2)} MB`;
const prereqsOk = () => Array.isArray(state.prereqs) && state.prereqs.every(p => p.ok);
const discOk = () => !!(state.disc.result && state.disc.result.ok);
// No Japanese disc is fine; a folder that was typed must check out (or be cleared) before going on.
const jpGiven = () => !!state.jp.path.trim();
const jpOk = () => !jpGiven() || !!(state.jp.result && state.jp.result.ok && !state.jp.checking);
const jpUsed = () => jpGiven() && jpOk();
const targetOk = () => !!(state.target.result && state.target.result.ok);

// ---- art pack ----------------------------------------------------------------------------
let musicStart = Promise.resolve(false);
function loadArt(present) {
  document.querySelectorAll('[data-art]').forEach(img => {
    if (!present) { img.classList.add('missing'); img.remove(); return; }
    img.addEventListener('error', () => { img.classList.add('missing'); }, { once: true });
    if (img.id === 'art-logo') img.addEventListener('load', () => document.body.classList.add('has-logo'), { once: true });
    img.src = `https://art.nukkoro.local/${img.dataset.art}`;
  });
  if (!present) return;
  // The setup program plays the music itself, so Windows shows it under the setup's own name.
  musicStart = native.request('music', 'start').then(reply => {
    if (reply.available) $('#music').hidden = false;
    return reply.available;
  }).catch(() => false);
  const music = action => native.request('music', action).catch(() => {});
  $('#music-on').addEventListener('change', e => music(e.target.checked ? 'play' : 'pause'));
  $('#music-volume').addEventListener('input', e => {
    music(`volume:${e.target.value}`);
    e.target.style.setProperty('--fill', `${e.target.value}%`);
    $('#music-value').textContent = `${e.target.value}%`;
  });
}

// ---- layout: one uniform scale for the stage, stretched backgrounds, art clipped to the window ----
function fit() {
  const width = window.innerWidth, height = window.innerHeight;
  const scale = Math.min(width / 1265, height / 648);
  const ox = (width - 1265 * scale) / 2, oy = (height - 648 * scale) / 2;
  $('#stage').style.transform = `translate(${ox}px, ${oy}px) scale(${scale})`;
  const edge = ox + 216 * scale;
  $('#bg-sidebar').style.width = `${edge}px`;
  $('#bg-main').style.left = `${edge}px`;
  // Visible window in stage coordinates.
  const view = { left: -ox / scale, top: -oy / scale, right: (width - ox) / scale, bottom: (height - oy) / scale };
  const clip = (id, area) => {
    const el = document.getElementById(id);
    if (!el) return;
    const x = el.offsetLeft, y = el.offsetTop, w = el.offsetWidth, h = el.offsetHeight;
    el.style.clipPath = `inset(${Math.max(0, area.top - y)}px ${Math.max(0, x + w - area.right)}px ${Math.max(0, y + h - area.bottom)}px ${Math.max(0, area.left - x)}px)`;
  };
  const main = { ...view, left: Math.max(216, view.left) };
  const sidebar = { ...view, right: 215 };
  clip('art-dooms-eye', main);
  clip('art-shadow', main);
  clip('art-eggman', { ...sidebar, top: 308, bottom: 462 });
  clip('art-commander', sidebar);
}

// ---- checks (through Backend.ps1) -----------------------------------------------------------
let discTimer = null, targetTimer = null, discSerial = 0, targetSerial = 0, jpTimer = null, jpSerial = 0;
function checkJapanese(delay = 400) {
  clearTimeout(jpTimer);
  const serial = ++jpSerial;   // at the edit, not when the timer fires: an answer for the old text is dropped
  state.jp.result = null;
  const path = state.jp.path.trim();
  if (!path) { state.jp.checking = false; updateStatus(); return; }
  if (!discOk()) { state.jp.checking = false; state.jp.result = { ok: false, message: 'Choose the PAL disc above first.' }; updateStatus(); return; }
  state.jp.checking = true;
  updateStatus();
  jpTimer = setTimeout(async () => {
    let result;
    if (serial !== jpSerial || !discOk()) return;
    try { result = await native.backend('check-jpdisc', path, state.disc.result.root); }
    catch (e) { result = { ok: false, message: e.message }; }
    if (serial !== jpSerial) return;
    state.jp.checking = false;
    // The disc is optional: a refusal says how to go on without it.
    if (!result.ok) result = { ...result, message: `${result.message} Or clear the field to go on with English voices.` };
    state.jp.result = result;
    if (result.ok && result.root && result.root !== path) {
      state.jp.path = result.root;
      const input = $('#jp-path');
      if (input) input.value = result.root;
    }
    updateStatus();
  }, delay);
}
function checkDisc(delay = 400) {
  clearTimeout(discTimer);
  const serial = ++discSerial;   // at the edit, not when the timer fires: an answer for the old text is dropped
  state.disc.result = null;
  state.target.result = null;
  const path = state.disc.path.trim();
  if (!path) { state.disc.checking = false; updateStatus(); return; }
  state.disc.checking = true;
  updateStatus();
  discTimer = setTimeout(async () => {
    let result;
    try { result = await native.backend('check-disc', path); }
    catch (e) { result = { ok: false, message: e.message }; }
    if (serial !== discSerial) return;
    state.disc.checking = false;
    state.disc.result = result;
    // A subfolder (sys, files) resolves to the extraction root.
    if (result.ok && result.root && result.root !== path) {
      state.disc.path = result.root;
      const input = $('#disc-path');
      if (input) input.value = result.root;
    }
    updateStatus();
    // The Japanese disc is checked against the PAL one (its voice files follow the PAL disc's).
    if (jpGiven()) checkJapanese(0);
  }, delay);
}
function checkTarget(delay = 300) {
  clearTimeout(targetTimer);
  const serial = ++targetSerial;   // at the edit, not when the timer fires: an answer for the old text is dropped
  state.target.result = null;
  const path = state.target.path.trim();
  if (!path || !discOk()) { state.target.checking = false; updateStatus(); return; }
  state.target.checking = true;
  updateStatus();
  targetTimer = setTimeout(async () => {
    let result;
    if (serial !== targetSerial || !discOk()) return;
    try { result = await native.backend('check-target', path, state.disc.result.root); }
    catch (e) { result = { ok: false, message: e.message }; }
    if (serial !== targetSerial) return;
    state.target.checking = false;
    state.target.result = result;
    updateStatus();
  }, delay);
}
async function checkPrereqs() {
  state.prereqs = null;
  // Only the Components page shows them; re-rendering another page would reset what the user is typing.
  const refresh = () => { if (state.step === 4) render(); else updateButtons(); };
  refresh();
  try { state.prereqs = await native.backend('prereqs'); }
  catch (e) { state.prereqs = [{ name: 'Build tools check', ok: false, detail: e.message, url: '' }]; }
  refresh();
}
async function browse(kind) {
  const isDisc = kind === 'disc';
  if (kind === 'jp') {
    try {
      const reply = await native.request('jpdisc-folder', state.jp.path);
      if (reply.path) { state.jp.path = reply.path; $('#jp-path').value = reply.path; checkJapanese(0); }
    } catch (e) { showDialog('Folder chooser', e.message, [{ label: 'OK' }]); }
    return;
  }
  const current = isDisc ? state.disc.path : state.target.path;
  try {
    const reply = await native.request(isDisc ? 'disc-folder' : 'folder', current);
    if (!reply.path) return;
    if (isDisc) { state.disc.path = reply.path; $('#disc-path').value = reply.path; checkDisc(0); }
    else { state.target.path = reply.path; $('#target-path').value = reply.path; checkTarget(0); }
  } catch (e) { showDialog('Folder chooser', e.message, [{ label: 'OK' }]); }
}

// ---- pages ------------------------------------------------------------------------------------
function statusLine(id, entry, idle) {
  if (entry.checking) return `<p class="status busy" id="${id}">Checking...</p>`;
  if (!entry.result) return `<p class="status" id="${id}">${escapeText(idle)}</p>`;
  return `<p class="status ${entry.result.ok ? 'ok' : 'bad'}" id="${id}">${escapeText(entry.result.message)}</p>`;
}
function updateStatus() {
  const disc = $('#disc-status'), target = $('#target-status'), jp = $('#jp-status');
  if (disc) disc.outerHTML = statusLine('disc-status', state.disc, '');
  if (target) target.outerHTML = statusLine('target-status', state.target, '');
  if (jp) jp.outerHTML = statusLine('jp-status', state.jp, jpIdle);
  const speak = $('#speak-row');
  if (speak) speak.hidden = !jpUsed() || !state.jp.result;
  updateButtons();
}
function check(key, label, extra = '', disabled = false) {
  return `<label class="check"><input type="checkbox" data-key="${key}" ${state[key] || disabled ? 'checked' : ''} ${disabled ? 'disabled' : ''}>` +
         `<span class="box"></span><span>${label}${extra ? ` <small>${extra}</small>` : ''}</span></label>`;
}

function renderWelcome() {
  content.innerHTML = `
    <h1>Welcome</h1>
    <p class="description">This builds Shadow the Hedgehog for PC from your own PAL disc (GUPP8P) and installs it. It is a static recompilation and still a work in progress.</p>
    <p class="hint">Dolphin: game › Properties › Filesystem › right-click the disc › Extract Entire Disc.</p>
    <label class="field-label" for="disc-path">Extracted disc folder</label>
    <div class="folder-row"><input id="disc-path" spellcheck="false" placeholder="Choose the folder Dolphin extracted the disc into" value="${escapeText(state.disc.path)}">
      <button class="button" id="disc-browse">Browse...</button></div>
    ${statusLine('disc-status', state.disc, 'It holds the sys and files folders. Nothing in it is changed.')}`;
  $('#disc-path').addEventListener('input', e => { state.disc.path = e.target.value; checkDisc(); });
  $('#disc-browse').addEventListener('click', () => browse('disc'));
}
function renderJapanese() {
  content.innerHTML = `
    <h1>Japanese Voices <small class="optional">optional</small></h1>
    <div class="card caution"><strong>Caution:</strong> the PAL disc has no Japanese voices for the events (cutscenes) and dialogue.
      For them to play in Japanese, also extract your Japanese disc (GUPJ8P) into a <em>different</em> folder and choose it below.
      Without it the game speaks English.</div>
    <label class="field-label" for="jp-path">Extracted Japanese disc folder</label>
    <div class="folder-row"><input id="jp-path" spellcheck="false" placeholder="Leave empty for English voices" value="${escapeText(state.jp.path)}">
      <button class="button" id="jp-browse">Browse...</button></div>
    ${statusLine('jp-status', state.jp, jpIdle)}
    <div id="speak-row" ${jpUsed() && state.jp.result ? '' : 'hidden'}>${check('speakJapanese', 'Speak Japanese', '(changeable later: voice in options.cfg)')}</div>`;
  $('#jp-path').addEventListener('input', e => { state.jp.path = e.target.value; checkJapanese(); });
  $('#jp-browse').addEventListener('click', () => browse('jp'));
  bindChecks();
}
const jpIdle = 'Its voice files are copied to the voice_jp folder of the game. Nothing in it is changed.';
function renderLocation() {
  content.innerHTML = `
    <h1>Install Location</h1>
    <p class="description">Choose the folder where Shadow the Hedgehog Recompiled will be installed. Your settings and saves are kept there.</p>
    <div class="folder-row" style="margin-top:16px"><input id="target-path" spellcheck="false" value="${escapeText(state.target.path)}">
      <button class="button" id="target-browse">Browse...</button></div>
    ${statusLine('target-status', state.target, '')}
    ${check('launch', 'Launch the game after installation')}`;
  $('#target-path').addEventListener('input', e => { state.target.path = e.target.value; checkTarget(); });
  $('#target-browse').addEventListener('click', () => browse('target'));
  bindChecks();
  if (!state.target.result && !state.target.checking) checkTarget(0);
}
// The build tools: present, or installed by the setup as the first step of the install (prereq.fix says what it
// fetches), or - only when the setup cannot do it - a link to the website.
const toolsAuto = () => Array.isArray(state.prereqs) ? state.prereqs.filter(p => !p.ok && p.fix) : [];
const toolsReady = () => Array.isArray(state.prereqs) && state.prereqs.every(p => p.ok || p.fix);
function renderComponents() {
  const tools = state.prereqs === null
    ? '<div class="busy">Checking what your PC has...</div>'
    : state.prereqs.map(p => p.ok
        ? `<div><span class="mark ok">✔</span><span>${escapeText(p.name)}</span></div>`
        : p.fix
          ? `<div><span class="mark auto">↓</span><span>${escapeText(p.name)}: setup installs it for you</span></div>`
          : `<div><span class="mark bad">✖</span><span>${escapeText(p.name)}</span>${p.url ? ` <a data-url="${escapeText(p.url)}">Website</a>` : ''}</div>` +
            (p.detail ? `<div class="tool-detail">${escapeText(p.detail)}</div>` : '')).join('');
  const auto = toolsAuto();
  const assets = (discOk() ? state.disc.result.assetBytes : 0) + (jpUsed() && state.jp.result ? state.jp.result.bytes || 0 : 0);
  // Tools the setup installs itself: one plain card instead of the list (nothing for the player to do or decide).
  const body = auto.length && toolsReady()
    ? `<div class="card compact auto-tools"><strong>Setup takes care of the rest.</strong> Your PC is missing free tools that building the game needs.
        When you select Install, setup downloads and installs them first (from Microsoft and python.org; 10-40 minutes the first time).
        When Windows asks for permission, choose Yes. Licence terms:
        <a data-url="https://visualstudio.microsoft.com/license-terms/">Microsoft</a>, <a data-url="https://www.python.org/about/legal/">Python</a>.</div>`
    : `<div class="tools compact"><strong>What the build needs</strong> ${toolsReady() ? '' : '<a id="recheck">Check again</a>'}${tools}</div>` +
      (prereqsOk() || state.prereqs === null
        ? `<div class="card compact"><strong>Space:</strong> ${assets ? size(assets + 150 * 1024 ** 2) : '-'} in the install folder. While it builds, setup uses about 1.5 GB more and frees it again when it is done.</div>` : '');
  content.innerHTML = `
    <h1>Components</h1>
    ${check('game', 'Game, built from your disc', '(required)', true)}
    ${check('desktop', 'Desktop shortcut')}
    ${check('startMenu', 'Start menu shortcut')}
    ${body}`;
  bindChecks();
  content.querySelectorAll('a[data-url]').forEach(a => a.addEventListener('click', () => native.request('open-url', a.dataset.url).catch(() => {})));
  const recheck = $('#recheck');
  if (recheck) recheck.addEventListener('click', checkPrereqs);
}
function renderReview() {
  const options = [state.desktop && 'Desktop shortcut', state.startMenu && 'Start menu shortcut', state.launch && 'Launch after installation'].filter(Boolean);
  const target = state.target.result || {};
  content.innerHTML = `
    <h1>Ready to Install</h1>
    <p class="description">${toolsAuto().length ? 'Select Install. Setup first installs the build tools your PC lacks (Windows asks for permission once), then builds the game.' : 'Review your settings, then select Install. The first build takes about 5-20 minutes.'}</p>
    <dl class="summary">
      <dt>Disc</dt><dd title="${escapeText(state.disc.result.root)}">${escapeText(state.disc.result.root)}</dd>
      <dt>Voices</dt><dd${jpUsed() ? ` title="${escapeText(state.jp.result.root)}"` : ''}>${jpUsed()
        ? `${state.speakJapanese ? 'Japanese' : 'English (Japanese voices copied too)'} · ${escapeText(state.jp.result.root)}`
        : 'English (no Japanese disc)'}</dd>
      <dt>Destination</dt><dd title="${escapeText(target.path)}">${escapeText(target.path)}</dd>
      <dt>Options</dt><dd>${escapeText(options.join(' · ') || 'None')}</dd>
      <dt>Space</dt><dd>${size((target.neededBytes || 0) + (jpUsed() ? state.jp.result.bytes || 0 : 0))}${target.existing ? ' (updates the existing install; saves are kept)' : ''}</dd>
    </dl>`;
}
// While installing, a spinner and a self-running clock show the install is alive even when no event
// arrives for minutes (one very large compile can keep step 2 silent for a long time).
const activity = { startedAt: 0, movedAt: 0, timer: 0 };
const STALL_MS = 20000;
const stalledText = 'The largest part of the game is still compiling. This can take several minutes; the bar will move again when it is done.';
const clock = ms => { const s = Math.floor(ms / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
function activityTick() {
  const elapsed = $('#elapsed');
  if (elapsed) elapsed.textContent = `${state.cancelling ? 'Stopping...' : 'Working...'} ${clock(Date.now() - activity.startedAt)}`;
  const description = $('#progress-page .description');
  if (description && state.phase === 'running') description.textContent = progressDescription();
}
function syncActivityTimer() {
  const running = state.phase === 'running';
  if (running && !activity.timer) activity.timer = setInterval(activityTick, 1000);
  else if (!running && activity.timer) { clearInterval(activity.timer); activity.timer = 0; }
}
function progressDescription() {
  if (state.stageIndex < 0) return 'Installing the build tools your PC lacks. If Windows asks for permission, choose Yes. This can take a while; you can keep using your computer.';
  if (state.stageIndex === 1 && !state.cancelling && Date.now() - activity.movedAt > STALL_MS) return stalledText;
  return 'Building the game from your disc and copying it. You can keep using your computer.';
}
function renderProgress() {
  const titles = { running: state.cancelling ? 'Cancelling...' : 'Installing...', complete: 'Installation Complete', failed: 'Installation Failed', cancelled: 'Installation Cancelled' };
  const descriptions = {
    running: escapeText(progressDescription()),
    complete: `Shadow the Hedgehog Recompiled is installed in ${escapeText(state.target.result.path)}.`,
    failed: `${escapeText(state.error)} Select Retry to try again.`,
    cancelled: 'The installation was stopped. Select Retry to start again.',
  };
  const percent = Math.round(state.progress * 100);
  const operation = state.stageName ? (state.stageIndex < 0 ? `Before the build · ${state.stageName}` : `Part ${state.stageIndex + 1} of 5 · ${state.stageName}`) : 'Starting...';
  const logText = state.log.slice(-20000) + (state.logPath && state.phase === 'failed' ? `\nFull log: ${state.logPath}` : '');
  const page = $('#progress-page');
  if (page && page.dataset.phase === state.phase) {
    // Same page already showing: update it in place so the spinner keeps turning smoothly.
    $('#progress-page h1').textContent = titles[state.phase];
    $('#progress-page .progress-track').setAttribute('aria-valuenow', percent);
    $('#progress-page .progress-fill').style.width = `${percent}%`;
    $('#operation-text').textContent = operation;
    $('#percent').textContent = `${percent}%`;
    const log = $('#log');
    if (log.textContent !== logText) { log.textContent = logText; log.scrollTop = log.scrollHeight; }
    if (state.phase === 'running') activityTick();
    else $('#progress-page .description').innerHTML = descriptions[state.phase];
    return;
  }
  const running = state.phase === 'running';
  content.innerHTML = `<div id="progress-page" data-phase="${state.phase}">
    <h1>${titles[state.phase]}</h1>
    <p class="description">${descriptions[state.phase]}</p>
    <div class="progress-track" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${percent}"><div class="progress-fill" style="width:${percent}%"></div></div>
    <div class="progress-details"><span id="operation">${running ? '<span class="spinner" aria-hidden="true"></span>' : ''}<span id="operation-text">${escapeText(operation)}</span></span><span class="progress-right">${running ? '<span id="elapsed"></span>' : ''}<span id="percent">${percent}%</span></span></div>
    <div class="log" id="log">${escapeText(logText)}</div></div>`;
  if (running) activityTick();
  const log = $('#log');
  log.scrollTop = log.scrollHeight;
}
function bindChecks() {
  content.querySelectorAll('.check input[data-key]').forEach(input => input.addEventListener('change', () => { state[input.dataset.key] = input.checked; }));
}

function render() {
  document.querySelectorAll('.step').forEach(button => {
    const n = Number(button.dataset.step);
    if (n === state.step) button.setAttribute('aria-current', 'step'); else button.removeAttribute('aria-current');
    button.disabled = state.phase === 'running' || state.phase === 'complete' || !canReach(n);
  });
  if (state.step === 1) renderWelcome();
  else if (state.step === 2) renderJapanese();
  else if (state.step === 3) renderLocation();
  else if (state.step === 4) renderComponents();
  else if (state.phase === 'review') renderReview();
  else renderProgress();
  syncActivityTimer();
  updateButtons();
}
function updateButtons() {
  const running = state.phase === 'running';
  back.textContent = running ? 'Cancel' : 'Back';
  back.disabled = (state.step === 1 && !running) || state.phase === 'complete' || state.cancelling;
  next.textContent = state.phase === 'complete' ? 'Finish' : state.step < 5 ? 'Next'
    : state.phase === 'failed' || state.phase === 'cancelled' ? 'Retry' : 'Install';
  next.disabled = running || (state.step < 5 && !canReach(state.step + 1));
}
function canReach(step) {
  if (step <= 1) return true;
  if (!discOk()) return false;
  if (step === 2) return true;
  if (!jpOk()) return false;
  if (step === 3) return true;
  if (!targetOk()) return false;
  if (step === 4) return true;
  return toolsReady();
}
function go(step) {
  if (state.phase === 'running' || state.phase === 'complete' || !canReach(step)) return;
  state.step = step;
  if (step < 5) state.phase = 'review';
  if (step === 4 && state.prereqs === null) checkPrereqs();
  render();
}

// ---- install ----------------------------------------------------------------------------------
async function startInstall() {
  Object.assign(state, { phase: 'running', progress: 0, stageIndex: 0, stageName: '', log: '', error: '', logPath: '', cancelling: false });
  activity.startedAt = activity.movedAt = Date.now();
  render();
  const japanese = jpUsed() ? state.jp.result.root : '-';
  const voice = jpUsed() ? (state.speakJapanese ? '0' : '1') : '-';
  const payload = [state.disc.result.root, state.target.result.path, state.desktop ? 1 : 0, state.startMenu ? 1 : 0, japanese, voice].join('\t');
  try { await native.request('install', payload); }
  catch (e) { state.phase = 'failed'; state.error = e.message; render(); }
}
let renderQueued = false;
function queueRender() { if (!renderQueued) { renderQueued = true; requestAnimationFrame(() => { renderQueued = false; render(); }); } }
native.on(message => {
  if (message.type === 'install') {
    const e = message.event || {};
    if (e.type === 'stage') { state.stageIndex = e.index; state.stageName = e.name; activity.movedAt = Date.now(); }
    else if (e.type === 'progress') {
      if (Math.round(e.value * 100) > Math.round(state.progress * 100)) activity.movedAt = Date.now();
      state.progress = Math.max(state.progress, e.value);
    }
    else if (e.type === 'log') state.log = (state.log + e.text).slice(-60000);
    else if (e.type === 'error') { state.error = e.message; state.logPath = e.log || ''; }
    else if (e.type === 'done') state.progress = 1;
    if (state.step === 5) queueRender();
  } else if (message.type === 'install-exit') {
    state.phase = message.code === 0 ? 'complete' : state.cancelling ? 'cancelled' : 'failed';
    if (state.phase === 'failed' && !state.error) state.error = `The installer stopped (code ${message.code}).`;
    state.cancelling = false;
    render();
    if (selftestHooks.onExit) selftestHooks.onExit();
  }
});
function cancelInstall() {
  state.cancelling = true;
  render();
  native.request('cancel-install').catch(() => {});
}
async function finish() {
  if (state.launch) {
    try { await native.request('launch', state.target.result.path); }
    catch (e) { await showDialog('Could not start the game', e.message, [{ label: 'Close' }]); }
  }
  native.send('close');
}

// ---- dialogs --------------------------------------------------------------------------------
function showDialog(title, text, buttons) {
  return new Promise(resolve => {
    const modal = $('#modal');
    modal.innerHTML = `<section class="dialog" role="dialog" aria-modal="true"><h2>${escapeText(title)}</h2><p>${escapeText(text)}</p><div class="actions">` +
      buttons.map((b, i) => `<button class="button" data-index="${i}">${escapeText(b.label)}</button>`).join('') + '</div></section>';
    modal.hidden = false;
    modal.querySelectorAll('button').forEach(button => button.addEventListener('click', () => {
      modal.hidden = true; modal.innerHTML = '';
      resolve(buttons[Number(button.dataset.index)].value);
    }));
    modal.querySelector('button:last-child').focus();
  });
}
// Called by the host when the window's close button is pressed.
window.requestClose = async () => {
  if (state.phase === 'complete') { native.send('close'); return; }
  const running = state.phase === 'running';
  const answer = await showDialog(running ? 'Stop the installation?' : 'Close setup?',
    running ? 'The build stops and the game is not installed.' : 'Nothing has been installed yet.',
    [{ label: 'Keep open', value: false }, { label: running ? 'Stop and close' : 'Close', value: true }]);
  if (answer) native.send('close');
};

// ---- wiring ----------------------------------------------------------------------------------
back.addEventListener('click', async () => {
  if (state.phase === 'running') {
    const answer = await showDialog('Cancel the installation?', 'The build stops. You can start it again with Retry.',
      [{ label: 'Keep going', value: false }, { label: 'Cancel installation', value: true }]);
    if (answer && state.phase === 'running') cancelInstall();
    return;
  }
  if (state.step === 5 && state.phase !== 'review') state.phase = 'review';
  go(state.step - 1);
});
next.addEventListener('click', () => {
  if (state.phase === 'complete') finish();
  else if (state.step < 5) go(state.step + 1);
  else startInstall();
});
document.querySelectorAll('.step').forEach(button => button.addEventListener('click', () => go(Number(button.dataset.step))));
window.addEventListener('resize', fit);
window.addEventListener('pagehide', () => { clearInterval(activity.timer); activity.timer = 0; });

// ---- self-test (NukkoroSetup.exe --selftest): drive every page and snapshot it -----------------
const selftestHooks = {};
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(condition, timeout = 30000) {
  const end = Date.now() + timeout;
  while (!condition()) { if (Date.now() > end) throw new Error('timed out'); await wait(100); }
}
async function selftest() {
  const report = { pages: [], passed: false };
  const snap = async name => { await wait(400); await native.request('selftest-snapshot', name); report.pages.push(name); };
  try {
    const config = await native.request('selftest-config');
    const musicExpected = await musicStart;
    const musicState = () => native.request('music', 'state');
    report.musicPlaying = false;
    for (let end = Date.now() + 10000; musicExpected && Date.now() < end; await wait(100)) {
      const state = await musicState();
      if (state.playing && state.positionMs > 0) { report.musicPlaying = true; break; }
    }
    await snap('1-welcome-empty');
    state.disc.path = 'C:\\Windows'; render(); checkDisc(0);
    await until(() => state.disc.result); report.badDiscRejected = !discOk();
    await snap('1-welcome-invalid');
    state.disc.path = config.disc; render(); checkDisc(0);
    await until(() => state.disc.result); report.discAccepted = discOk(); report.discRoot = state.disc.result.root;
    await snap('1-welcome-valid');
    next.click(); await wait(100);
    await snap('2-japanese-empty');
    state.jp.path = config.disc; render(); checkJapanese(0);
    await until(() => state.jp.result && !state.jp.checking); report.palAsJapaneseRefused = !jpOk() && !canReach(3);
    await snap('2-japanese-refused');
    if (config.jpdisc) {
      state.jp.path = config.jpdisc; render(); checkJapanese(0);
      await until(() => state.jp.result && !state.jp.checking); report.japaneseAccepted = jpUsed() && canReach(3);
      await snap('2-japanese-valid');
    } else { state.jp.path = ''; render(); checkJapanese(0); }
    next.click(); await wait(100);
    state.target.path = config.target; render(); checkTarget(0);
    await until(() => state.target.result); report.targetAccepted = targetOk();
    await snap('3-location');
    next.click();
    await until(() => state.prereqs !== null); render(); report.prereqsOk = prereqsOk();
    await snap('4-components');
    report.toolsAuto = toolsAuto().map(p => p.key); report.toolsReady = toolsReady();
    state.desktop = false; state.startMenu = false; state.launch = false;
    next.click(); await wait(100);
    await snap('5-ready');
    // Preview of the install page during a long silent compile (no install runs): the spinner must
    // survive re-renders, and it and its timer must go away once the install has ended.
    Object.assign(state, { phase: 'running', stageIndex: 1, stageName: 'Translate and compile the game', progress: 0.84,
                           log: 'Building the game...\n' });
    activity.startedAt = Date.now() - 222000; activity.movedAt = Date.now() - STALL_MS - 5000;
    render();
    const spinner = $('.spinner');
    state.log += 'still compiling\n'; render(); await wait(1200);
    report.spinnerStable = !!spinner && $('.spinner') === spinner && !!activity.timer;
    report.stalledNoteShown = $('#progress-page .description').textContent === stalledText;
    await snap('5-installing-preview');
    state.phase = 'cancelled'; render();
    report.spinnerGone = !$('.spinner') && !activity.timer;
    Object.assign(state, { phase: 'review', progress: 0, stageIndex: 0, stageName: '', log: '' }); render();
    report.musicLoops = (await musicState()).loops;
    report.passed = report.badDiscRejected && report.discAccepted && report.palAsJapaneseRefused &&
                    (!config.jpdisc || report.japaneseAccepted) && report.targetAccepted && report.toolsReady &&
                    (!musicExpected || report.musicPlaying) && report.spinnerStable && report.stalledNoteShown && report.spinnerGone;
    if (config.install && report.passed) {
      const exited = new Promise(resolve => { selftestHooks.onExit = resolve; });
      next.click();
      await until(() => state.phase === 'running', 5000);
      await wait(4000); await snap('5-installing');
      await exited;
      await snap(`5-${state.phase}`);
      report.installPhase = state.phase;
      report.passed = state.phase === 'complete';
    }
  } catch (e) { report.error = String(e.message || e); }
  native.request('selftest-done', JSON.stringify(report)).catch(() => {});
}

// ---- start ---------------------------------------------------------------------------------
(async () => {
  fit();
  let info = { art: false, selftest: false };
  try { info = await native.request('ping'); } catch { /* opened outside the host: art and checks stay off */ }
  loadArt(info.art);
  fit();
  render();
  try { state.target.path = (await native.backend('defaults')).target; } catch { state.target.path = ''; }
  checkPrereqs();
  if (info.selftest || new URLSearchParams(location.search).has('selftest')) selftest();
})();
