// Shadow the Hedgehog uninstall page, in the setup's look. Three steps: Welcome (what is installed, and what goes),
// Your Data (saves, settings, mods and snapshots: kept unless ticked), Uninstall (review, then Uninstall.ps1 through the
// host, with its progress).
'use strict';

// ---- native bridge (as the setup's) ----------------------------------------------------------
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
      if (!view) return Promise.reject(new Error('This page must run inside the uninstall program.'));
      return new Promise((resolve, reject) => {
        const id = ++nextId;
        pending.set(id, { resolve, reject });
        view.postMessage(`${command}|${id}|${payload}`);
      });
    },
    send(message) { if (view) view.postMessage(message); },
    on(fn) { listeners.push(fn); },
  };
})();

// ---- state -------------------------------------------------------------------------------------
const state = {
  step: 1,
  plan: null,            // Uninstall.ps1 -Plan: { ok, path, game, voices, shortcuts, apps_entry, data[], other, running }
  planError: '',
  remove: {},            // data kind -> true when ticked
  phase: 'review',       // review | running | complete | failed
  progress: 0, stageName: '', log: '', error: '', result: null,
};
const $ = selector => document.querySelector(selector);
const content = $('#content'), back = $('#back'), next = $('#next');
const escapeText = value => String(value ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const size = bytes => bytes >= 1024 ** 3 ? `${(bytes / 1024 ** 3).toFixed(1)} GB`
  : bytes >= 1024 ** 2 ? `${Math.round(bytes / 1024 ** 2)} MB` : bytes > 0 ? `${Math.max(1, Math.round(bytes / 1024))} KB` : '0 KB';
const planOk = () => !!(state.plan && state.plan.ok);
const removing = () => planOk() ? state.plan.data.filter(d => d.present && state.remove[d.key]) : [];
const keeping = () => planOk() ? state.plan.data.filter(d => d.present && !state.remove[d.key]) : [];

// ---- art pack and layout (as the setup's) ------------------------------------------------------
let musicStart = Promise.resolve(false);
function loadArt(present) {
  document.querySelectorAll('[data-art]').forEach(img => {
    if (!present) { img.classList.add('missing'); img.remove(); return; }
    img.addEventListener('error', () => { img.classList.add('missing'); }, { once: true });
    if (img.id === 'art-logo') img.addEventListener('load', () => document.body.classList.add('has-logo'), { once: true });
    img.src = `https://art.nukkoro.local/${img.dataset.art}`;
  });
  if (!present) return;
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
function fit() {
  const width = window.innerWidth, height = window.innerHeight;
  const scale = Math.min(width / 1265, height / 648);
  const ox = (width - 1265 * scale) / 2, oy = (height - 648 * scale) / 2;
  $('#stage').style.transform = `translate(${ox}px, ${oy}px) scale(${scale})`;
  const edge = ox + 216 * scale;
  $('#bg-sidebar').style.width = `${edge}px`;
  $('#bg-main').style.left = `${edge}px`;
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
  // The sidebar has three steps: Eggman shows from just below them.
  clip('art-eggman', { ...sidebar, top: 196, bottom: 462 });
  clip('art-commander', sidebar);
}

// ---- the plan ------------------------------------------------------------------------------------
async function loadPlan() {
  state.plan = null; state.planError = '';
  render();
  try {
    const reply = await native.request('uninstall-plan');
    state.plan = reply.result;
    if (!state.plan.ok) state.planError = state.plan.message || 'This folder cannot be uninstalled.';
  } catch (e) { state.planError = e.message; }
  render();
}

// ---- pages ---------------------------------------------------------------------------------------
function item(mark, what, bytes = '') {
  const cls = mark === '✖' ? 'remove' : mark === '✔' ? 'keep' : '';
  return `<div><span class="mark ${cls}">${mark}</span><span class="what">${what}</span><span class="bytes">${bytes}</span></div>`;
}
function renderWelcome() {
  if (!state.plan && !state.planError) {
    content.innerHTML = `<h1>Uninstall</h1><p class="description">Looking at what is installed...</p>
      <div class="progress-details"><span id="operation"><span class="spinner" aria-hidden="true"></span><span>Reading the install record</span></span></div>`;
    return;
  }
  if (!planOk()) {
    content.innerHTML = `<h1>Uninstall</h1>
      <div class="card caution"><strong>This folder cannot be uninstalled from here.</strong> ${escapeText(state.planError)}</div>`;
    return;
  }
  const p = state.plan;
  const rows = [
    item('✖', 'The game, built from your disc, with Settings and the Mod Manager', size(p.game.bytes)),
    p.voices.files ? item('✖', 'The Japanese voices', size(p.voices.bytes)) : '',
    p.shortcuts.length || p.apps_entry ? item('✖', [p.shortcuts.length && `Its shortcuts (${p.shortcuts.length})`, p.apps_entry && "its entry in Windows' Installed apps"].filter(Boolean).join(' and ')) : '',
  ].join('');
  content.innerHTML = `
    <h1>Uninstall</h1>
    <p class="description">This removes Shadow the Hedgehog from <span class="path">${escapeText(p.path)}</span>.</p>
    <div class="items">${rows}</div>
    ${p.running ? '<div class="card caution"><strong>The game is running.</strong> Close it, then <a class="link" id="recheck">check again</a>.</div>'
                : '<p class="hint">Your saves, settings and mods stay unless you choose otherwise on the next page.</p>'}`;
  const recheck = $('#recheck');
  if (recheck) recheck.addEventListener('click', loadPlan);
}
function renderData() {
  const p = state.plan;
  const rows = p.data.map(d => `<label class="check"><input type="checkbox" data-kind="${d.key}" ${state.remove[d.key] ? 'checked' : ''} ${d.present ? '' : 'disabled'}>` +
    `<span class="box"></span><span>Delete my ${escapeText(d.name.toLowerCase())}</span><span class="bytes">${d.present ? size(d.bytes) : 'none'}</span></label>`).join('');
  content.innerHTML = `
    <h1>Your Data</h1>
    <p class="description">Everything you made with the game is kept unless you tick it here, so you can install again later and carry on.</p>
    <div class="data">${rows}</div>
    ${p.other.files ? `<p class="hint">${p.other.files} other file${p.other.files === 1 ? '' : 's'} (${size(p.other.bytes)}) that setup did not put in the folder stay too.</p>` : ''}`;
  content.querySelectorAll('input[data-kind]').forEach(input => input.addEventListener('change', () => { state.remove[input.dataset.kind] = input.checked; }));
}
function renderReview() {
  const p = state.plan;
  const gone = removing(), stay = keeping();
  const rows = [
    item('✖', 'The game, its files and shortcuts', size(p.game.bytes + p.voices.bytes)),
    ...gone.map(d => item('✖', `Your ${escapeText(d.name.toLowerCase())}`, size(d.bytes))),
    ...stay.map(d => item('✔', `Your ${escapeText(d.name.toLowerCase())} stay`, size(d.bytes))),
  ].join('');
  content.innerHTML = `
    <h1>Ready to Uninstall</h1>
    <p class="description">Review what happens, then select Uninstall.${gone.length ? ' Deleted data cannot be brought back.' : ''}</p>
    <div class="items">${rows}</div>`;
}
const activity = { startedAt: 0, timer: 0 };
const clock = ms => { const s = Math.floor(ms / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; };
function activityTick() { const el = $('#elapsed'); if (el) el.textContent = `Working... ${clock(Date.now() - activity.startedAt)}`; }
function syncActivityTimer() {
  const running = state.phase === 'running';
  if (running && !activity.timer) activity.timer = setInterval(activityTick, 1000);
  else if (!running && activity.timer) { clearInterval(activity.timer); activity.timer = 0; }
}
function completeText() {
  const r = state.result || {};
  if (r.folder_removed) return `Shadow the Hedgehog has been removed, and so has its folder.`;
  const left = (r.left || []).map(name => name.toLowerCase());
  const list = left.length > 1 ? `${left.slice(0, -1).join(', ')} and ${left[left.length - 1]}` : left.join('');
  return `Shadow the Hedgehog has been removed.${left.length ? ` Your ${escapeText(list)} stay in <span class="path">${escapeText(r.path || state.plan.path)}</span>.` +
    ` <a class="link" id="open-folder">Open the folder</a>` : ' Files that setup did not put there stay in the folder.'}`;
}
function renderProgress() {
  const titles = { running: 'Uninstalling...', complete: 'Uninstall Complete', failed: 'Uninstall Failed' };
  const descriptions = {
    running: 'Removing the game. This takes a few seconds.',
    complete: completeText(),
    failed: `${escapeText(state.error)} Select Retry to try again.`,
  };
  const percent = Math.round(state.progress * 100);
  const page = $('#progress-page');
  if (page && page.dataset.phase === state.phase) {
    $('#progress-page .progress-fill').style.width = `${percent}%`;
    $('#progress-page .progress-track').setAttribute('aria-valuenow', percent);
    $('#operation-text').textContent = state.stageName || 'Starting...';
    $('#percent').textContent = `${percent}%`;
    const log = $('#log');
    if (log.textContent !== state.log) { log.textContent = state.log; log.scrollTop = log.scrollHeight; }
    return;
  }
  const running = state.phase === 'running';
  content.innerHTML = `<div id="progress-page" data-phase="${state.phase}">
    <h1>${titles[state.phase]}</h1>
    <p class="description">${descriptions[state.phase]}</p>
    <div class="progress-track" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${percent}"><div class="progress-fill" style="width:${percent}%"></div></div>
    <div class="progress-details"><span id="operation">${running ? '<span class="spinner" aria-hidden="true"></span>' : ''}<span id="operation-text">${escapeText(state.stageName || 'Starting...')}</span></span><span class="progress-right">${running ? '<span id="elapsed"></span>' : ''}<span id="percent">${percent}%</span></span></div>
    <div class="log" id="log">${escapeText(state.log)}</div></div>`;
  if (running) activityTick();
  const log = $('#log');
  log.scrollTop = log.scrollHeight;
  const open = $('#open-folder');
  if (open) open.addEventListener('click', () => native.request('open-folder').catch(e => showDialog('Open the folder', e.message, [{ label: 'OK' }])));
}

function render() {
  document.querySelectorAll('.step').forEach(button => {
    const n = Number(button.dataset.step);
    if (n === state.step) button.setAttribute('aria-current', 'step'); else button.removeAttribute('aria-current');
    button.disabled = state.phase !== 'review' || !canReach(n);
  });
  if (state.step === 1) renderWelcome();
  else if (state.step === 2) renderData();
  else if (state.phase === 'review') renderReview();
  else renderProgress();
  syncActivityTimer();
  updateButtons();
}
function updateButtons() {
  const running = state.phase === 'running';
  back.textContent = state.phase === 'complete' ? 'Close' : 'Back';
  back.hidden = state.phase === 'complete';
  back.disabled = state.step === 1 || running;
  next.textContent = state.phase === 'complete' ? 'Finish' : state.step < 3 ? 'Next' : state.phase === 'failed' ? 'Retry' : 'Uninstall';
  next.disabled = running || (state.phase === 'review' && !canReach(Math.min(3, state.step + 1)));
}
function canReach(step) {
  if (step <= 1) return true;
  return planOk() && !state.plan.running;
}
function go(step) {
  if (state.phase !== 'review' || !canReach(step)) return;
  state.step = step;
  render();
}

// ---- uninstall -----------------------------------------------------------------------------------
async function startUninstall() {
  const gone = removing();
  if (gone.length) {
    const names = gone.map(d => d.name.toLowerCase()).join(', ');
    const answer = await showDialog('Delete your data?', `Your ${names} will be deleted with the game. This cannot be undone.`,
      [{ label: 'Keep them', value: false }, { label: 'Delete', value: true }]);
    if (!answer) return;
  }
  Object.assign(state, { phase: 'running', progress: 0, stageName: '', log: '', error: '', result: null });
  activity.startedAt = Date.now();
  render();
  try { await native.request('uninstall', gone.map(d => d.key).join(',')); }
  catch (e) { state.phase = 'failed'; state.error = e.message; render(); }
}
let renderQueued = false;
function queueRender() { if (!renderQueued) { renderQueued = true; requestAnimationFrame(() => { renderQueued = false; render(); }); } }
native.on(message => {
  if (message.type === 'uninstall') {
    const e = message.event || {};
    if (e.type === 'stage') state.stageName = e.name;
    else if (e.type === 'progress') state.progress = Math.max(state.progress, e.value);
    else if (e.type === 'log') state.log = (state.log + e.text).slice(-60000);
    else if (e.type === 'error') state.error = e.message;
    else if (e.type === 'done') { state.result = e; state.progress = 1; state.stageName = 'Done'; }
    queueRender();
  } else if (message.type === 'uninstall-exit') {
    state.phase = message.code === 0 && state.result ? 'complete' : 'failed';
    if (state.phase === 'failed' && !state.error) state.error = `The uninstaller stopped (code ${message.code}).`;
    render();
    if (selftestHooks.onExit) selftestHooks.onExit();
  }
});

// ---- dialogs (as the setup's) ---------------------------------------------------------------------
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
    modal.querySelector('button:first-child').focus();
  });
}
// Called by the host when the window's close button is pressed. An uninstall is never stopped half way.
window.requestClose = async () => {
  if (state.phase === 'complete' || state.phase === 'failed' || !planOk()) { native.send('close'); return; }
  if (state.phase === 'running') {
    await showDialog('Almost done', 'The game is being removed. The window closes when you select Finish.', [{ label: 'OK' }]);
    return;
  }
  const answer = await showDialog('Close the uninstaller?', 'Nothing has been removed.',
    [{ label: 'Keep open', value: false }, { label: 'Close', value: true }]);
  if (answer) native.send('close');
};

// ---- wiring ----------------------------------------------------------------------------------------
back.addEventListener('click', () => { if (state.phase === 'failed') state.phase = 'review'; go(state.step - 1); });
next.addEventListener('click', () => {
  if (state.phase === 'complete') native.send('close');
  else if (state.step < 3) go(state.step + 1);
  else startUninstall();
});
document.querySelectorAll('.step').forEach(button => button.addEventListener('click', () => go(Number(button.dataset.step))));
window.addEventListener('resize', fit);
window.addEventListener('pagehide', () => { clearInterval(activity.timer); activity.timer = 0; });

// ---- self-test (Uninstall.exe --selftest <dir> [--install]): every page snapshotted; with --install, really
// uninstalls (keeping the data) --------------------------------------------------------------------------
const selftestHooks = {};
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(condition, timeout = 30000) {
  const end = Date.now() + timeout;
  while (!condition()) { if (Date.now() > end) throw new Error('timed out'); await wait(100); }
}
async function selftest() {
  const report = { pages: [], passed: false };
  const snap = async name => { await wait(500); await native.request('selftest-snapshot', name); report.pages.push(name); };
  try {
    const config = await native.request('selftest-config');
    await until(() => state.plan || state.planError);
    report.planOk = planOk();
    report.plan = state.plan;
    await snap('u1-welcome');
    if (!planOk()) throw new Error(state.planError);
    next.click(); await wait(100);
    await snap('u2-data');
    const present = state.plan.data.filter(d => d.present);
    if (present.length) { state.remove[present[0].key] = true; render(); await snap('u2-data-ticked'); state.remove[present[0].key] = false; render(); }
    next.click(); await wait(100);
    await snap('u3-ready');
    report.passed = state.step === 3 && state.phase === 'review';
    if (config.install && report.passed) {
      const exited = new Promise(resolve => { selftestHooks.onExit = resolve; });
      next.click();
      await exited;
      await snap(`u3-${state.phase}`);
      report.phase = state.phase; report.result = state.result; report.log = state.log;
      report.passed = state.phase === 'complete';
    }
  } catch (e) { report.error = String(e.message || e); }
  native.request('selftest-done', JSON.stringify(report)).catch(() => {});
}

// ---- start -------------------------------------------------------------------------------------------
(async () => {
  fit();
  let info = { art: false, selftest: false };
  try { info = await native.request('ping'); } catch { /* opened outside the program: no art, no plan */ }
  loadArt(info.art);
  fit();
  if (native.available) loadPlan(); else { state.planError = 'Open this page with Uninstall.exe.'; render(); }
  if (info.selftest) selftest();
})();
