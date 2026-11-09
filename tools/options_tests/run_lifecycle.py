#!/usr/bin/env python3
"""Real-game lifecycle runner for the Options system (settings-validation harness, Agent 5).

For each scenario it
  1. makes its own copy folder .build/options-menu/agent5-<scenario>/ from .build/options-menu/run (card, SRAM, pad.cfg;
     a windowed display.cfg so it never takes the user's screen) and writes the scenario's options.cfg (or none, or a
     corrupt one);
  2. takes the machine-wide lock .build/options-menu/RUN.lock (waits up to 20 min; never kills a game it did not
     start), launches .build/watch/bin/Release/ShadowTheHedgehog.exe with a generated --ui-test-script (boot -> main
     menu -> Options -> category -> change -> A -> back) and a --budget-ms bound plus a wall-clock timeout, and releases
     the lock when the process is gone;
  3. parses stdout (the end-of-run report: "Options Display tile", "Save store", "Stop reason", "Japanese voices", the
     "[ui-test] VI n: label" step lines), stderr and session-report.json, and reads the resulting options.cfg;
  4. checks the scenario's expectations (value persisted, no execution fault, no "guest call did not return", the
     script reached its labels, the Options screen was seen), then launches a SECOND time with a no-change script
     (boot -> Options -> Audio -> back) and checks that options.cfg is byte-identical and that the boot applied it
     ("options applied N" on the Save store line).

Usage (from the repository root, Python 3, no third-party modules):
    python tools/options_tests/run_lifecycle.py --list
    python tools/options_tests/run_lifecycle.py master_volume rumble          # named scenarios
    python tools/options_tests/run_lifecycle.py --all                         # every scenario, sequentially
    python tools/options_tests/run_lifecycle.py --dry-run --all               # write folders + scripts, no launch
Options: --exe PATH  --no-restart (skip the second launch)  --budget-extra-ms N  --keep (keep copy folders; default
    keeps them: they are small except card.raw, 16 MiB)  --lock-timeout-s N  --out-json FILE

Exit status 0 only when every selected scenario passed. A scenario that could not run (lock timeout, exe missing)
is reported as NOT RUN, never as passed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OPTS = os.path.join(ROOT, '.build', 'options-menu')
RUN_SRC = os.path.join(OPTS, 'run')
LOCK = os.path.join(OPTS, 'RUN.lock')
EXE = os.path.join(ROOT, '.build', 'watch', 'bin', 'Release', 'ShadowTheHedgehog.exe')
GAME_DATA = 'H:/ShadowTheHedgehogDiskGC_Extract/game'
AGENT = 'Agent5-lifecycle'

DEFAULT_CFG = 'language=1\nspeaker=0\nvibration=true\nautosave=true\nvoice=1\n'

# ---------------------------------------------------------------------------------------------------------------
# Script generation (runtime/include/gekko/ui_test.h format; waits are VI fields)
# ---------------------------------------------------------------------------------------------------------------

GRID = {'display': (0, 0), 'graphics': (0, 1), 'audio': (1, 0), 'controls': (1, 1), 'savedata': (2, 0), 'system': (2, 1)}
# Row indices of the page (the open category's settings, then its "Restore defaults" row).
ROWS = {
    'audio': {'master_volume': 0, 'speaker': 1, 'voice': 2, 'subtitles': 3, 'sound_test': 4, 'defaults': 5},
    'controls': {'vibration': 0, 'rumble': 1, 'invert_x': 2, 'invert_y': 3, 'defaults': 4},
    'savedata': {'load_game': 0, 'autosave': 1, 'defaults': 2},
    'system': {'restore_all': 0, 'build': 1},
}


class Script:
    def __init__(self):
        self.lines = []
        self.frames = 0
        self.cat = 'display'     # grid cursor
        self.row = 0             # page cursor
        self.in_page = False
        self.labels = []

    def _add(self, text, frames):
        self.lines.append(text)
        self.frames += frames

    def wait(self, n):
        self._add('wait %d' % n, n)

    def press(self, buttons, n=6):
        self._add('press %s %d' % (buttons, n), n)

    def label(self, name):
        self.lines.append('label ' + name)
        self.labels.append(name)

    def boot_to_options(self):
        """Hz screen -> boot dialog -> logos -> START -> main menu -> Options (the voice-test / options-tour prefix)."""
        self.label('hz-select'); self.wait(120); self.press('A', 8)
        self.label('boot-dialog'); self.wait(400); self.press('A', 8)
        self.label('logos'); self.wait(700); self.press('START', 8)
        self.label('main-menu'); self.wait(300)
        self.press('DOWN'); self.wait(40); self.press('DOWN'); self.wait(40); self.press('A', 8)
        self.label('options-grid'); self.wait(400)
        self.cat, self.row, self.in_page = 'display', 0, False

    def go_cat(self, cat):
        assert not self.in_page
        r0, c0 = GRID[self.cat]
        r1, c1 = GRID[cat]
        for _ in range(abs(r1 - r0)):
            self.press('DOWN' if r1 > r0 else 'UP'); self.wait(40)
        for _ in range(abs(c1 - c0)):
            self.press('RIGHT' if c1 > c0 else 'LEFT'); self.wait(40)
        self.cat = cat
        self.label('hover-' + cat)

    def open_page(self):
        assert not self.in_page
        self.press('A', 8); self.wait(300)
        self.in_page, self.row = True, 0
        self.label('page-' + self.cat)

    def to_row(self, name):
        assert self.in_page
        target = ROWS[self.cat][name]
        while self.row < target:
            self.press('DOWN'); self.wait(30); self.row += 1
        while self.row > target:
            self.press('UP'); self.wait(30); self.row -= 1

    def change(self, delta, times=1):
        for _ in range(times):
            self.press('RIGHT' if delta > 0 else 'LEFT'); self.wait(30)

    def apply(self, game_option):
        """A on the row: applies the whole category. A game option (speaker, vibration, language, voice) runs the
        game's own fade + apply (state 3) and returns to the grid; a PC-only value keeps the page open."""
        assert self.in_page
        self.label('apply-%s-%s' % (self.cat, 'game' if game_option else 'pc'))
        self.press('A', 8)
        if game_option:
            self.wait(900)
            self.in_page = False
        else:
            self.wait(300)

    def back(self):
        assert self.in_page
        self.press('B', 8); self.wait(120)
        self.in_page = False

    def finish(self, tail=300):
        if self.in_page:
            self.back()
        self.label('end')
        self.wait(tail)

    def text(self):
        return '\n'.join(['# generated by tools/options_tests/run_lifecycle.py'] + self.lines) + '\n'


# ---------------------------------------------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------------------------------------------

class Scenario:
    def __init__(self, name, description, build, cfg=DEFAULT_CFG, expect=None, kind='text', min_changes=0,
                 expect_fresh_file=False, extra_args=()):
        self.name, self.description, self.build = name, description, build
        self.cfg = cfg                  # text, bytes, or None (no options.cfg: a fresh configuration)
        self.expect = expect or {}      # options.cfg key -> expected value after run 1 ('true'/'false' for bools)
        self.min_changes = min_changes  # at least this many "changes" in the Options report line
        self.extra_args = list(extra_args)


def sc_master_volume(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.to_row('master_volume')
    s.change(-1, 4); s.apply(False); s.finish()


def sc_rumble(s):
    s.boot_to_options(); s.go_cat('controls'); s.open_page(); s.to_row('rumble')
    s.change(-1, 1); s.apply(False); s.finish()


def sc_invert(s):
    s.boot_to_options(); s.go_cat('controls'); s.open_page()
    s.to_row('invert_x'); s.change(+1); s.apply(False)
    s.to_row('invert_y'); s.change(+1); s.apply(False); s.finish()


def sc_speaker(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.to_row('speaker'); s.change(+1); s.apply(True); s.finish()


def sc_vibration(s):
    s.boot_to_options(); s.go_cat('controls'); s.open_page(); s.to_row('vibration'); s.change(-1); s.apply(True); s.finish()


def sc_subtitles(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.to_row('subtitles'); s.change(+1); s.apply(True)
    s.wait(600); s.finish()


def sc_autosave(s):
    s.boot_to_options(); s.go_cat('savedata'); s.open_page(); s.to_row('autosave'); s.change(-1); s.apply(False); s.finish()


def sc_restore_audio(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.to_row('defaults'); s.apply(True); s.finish()


def sc_restore_controls(s):
    s.boot_to_options(); s.go_cat('controls'); s.open_page(); s.to_row('defaults'); s.apply(True); s.finish()


def sc_restore_savedata(s):
    s.boot_to_options(); s.go_cat('savedata'); s.open_page(); s.to_row('defaults'); s.apply(False); s.finish()


def sc_restore_all(s):
    s.boot_to_options(); s.go_cat('system'); s.open_page(); s.to_row('restore_all')
    s.label('apply-system-restore-all'); s.press('A', 8); s.wait(1200)
    s.in_page = True
    s.finish()


def sc_fresh_boot(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.wait(200); s.finish()


def sc_fresh_change(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.to_row('master_volume'); s.change(-1, 2); s.apply(False); s.finish()


def sc_rapid(s):
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.to_row('master_volume')
    s.label('rapid-changes')
    for i in range(24):
        s.press('LEFT' if i % 3 else 'RIGHT', 2); s.wait(2)
    s.label('rapid-apply')
    for _ in range(4):
        s.press('A', 2); s.wait(2)
    s.wait(300)
    s.to_row('speaker')
    for i in range(12):
        s.press('RIGHT' if i % 2 == 0 else 'LEFT', 2); s.wait(2)
    s.wait(120)
    s.finish()


def sc_nav(s):
    s.boot_to_options()
    for rnd in range(3):
        for cat in ('graphics', 'audio', 'controls', 'savedata', 'system', 'display'):
            s.go_cat(cat); s.open_page(); s.wait(60)
            if s.cat == 'audio':
                s.to_row('master_volume')
            s.back()
    s.go_cat('audio'); s.open_page(); s.to_row('master_volume'); s.change(-1, 3); s.apply(False); s.finish()


CORRUPT = b'master_volume=ninety\nrumble_strength=7\nlanguage=42\n\x00\x01\xff garbage line\nunknown_key=1\nautosave=maybe\n'

SCENARIOS = [
    Scenario('master_volume', 'Audio > Master volume 100 -> 80, A; restart', sc_master_volume,
             expect={'master_volume': '80'}, min_changes=1),
    Scenario('rumble', 'Controls > Vibration strength 100 -> 75, A; restart', sc_rumble,
             expect={'rumble_strength': '75'}, min_changes=1),
    Scenario('camera_invert', 'Controls > Invert camera X and Y on, A each; restart', sc_invert,
             expect={'invert_camera_x': 'true', 'invert_camera_y': 'true'}, min_changes=2),
    Scenario('speaker', 'Audio > Speaker output stereo -> monaural, A (game fade + apply); restart', sc_speaker,
             expect={'speaker': '1'}, min_changes=1),
    Scenario('vibration', 'Controls > Vibration on -> off, A (game apply); restart', sc_vibration,
             expect={'vibration': 'false'}, min_changes=1),
    Scenario('subtitles', 'Audio > Subtitles English -> French, A (game state 3); restart', sc_subtitles,
             expect={'language': '2'}, min_changes=1),
    Scenario('autosave', 'Save Data > Autosave on -> off, A; restart', sc_autosave,
             expect={'autosave': 'false'}, min_changes=1),
    Scenario('restore_audio', 'Audio > Restore defaults (master 30, speaker 2 -> 100, 0; language kept)', sc_restore_audio,
             cfg='master_volume=30\nlanguage=3\nspeaker=2\nvibration=true\nautosave=true\nvoice=1\n',
             expect={'master_volume': '100', 'speaker': '0', 'language': '3'}),
    Scenario('restore_controls', 'Controls > Restore defaults (rumble 50, invert, vibration off -> defaults)',
             sc_restore_controls,
             cfg='rumble_strength=50\ninvert_camera_x=true\ninvert_camera_y=true\nlanguage=1\nspeaker=0\nvibration=false\n'
                 'autosave=true\nvoice=1\n',
             expect={'rumble_strength': '100', 'invert_camera_x': 'false', 'invert_camera_y': 'false',
                     'vibration': 'true'}),
    Scenario('restore_savedata', 'Save Data > Restore defaults (autosave off -> on)', sc_restore_savedata,
             cfg='language=1\nspeaker=0\nvibration=true\nautosave=false\nvoice=1\n', expect={'autosave': 'true'}),
    Scenario('restore_all', 'System > Restore all defaults (every category; languages kept)', sc_restore_all,
             cfg='master_volume=30\nrumble_strength=50\ninvert_camera_x=true\nlanguage=3\nspeaker=2\nvibration=false\n'
                 'autosave=false\nvoice=1\n',
             expect={'master_volume': '100', 'rumble_strength': '100', 'invert_camera_x': 'false', 'speaker': '0',
                     'vibration': 'true', 'autosave': 'true', 'language': '3'}),
    Scenario('fresh_boot', 'Fresh configuration (no options.cfg): boot and open Audio, change nothing', sc_fresh_boot,
             cfg=None),
    Scenario('fresh_change', 'Fresh configuration: change master volume, A; options.cfg is created', sc_fresh_change,
             cfg=None, expect={'master_volume': '90'}, min_changes=1),
    Scenario('corrupt_cfg', 'Corrupt options.cfg (bad values, NUL, unknown key): boot, change master volume, A',
             sc_fresh_change, cfg=CORRUPT, expect={'master_volume': '90'}, min_changes=1),
    Scenario('rapid', 'Rapid LEFT/RIGHT presses and repeated A (master volume, then speaker)', sc_rapid, min_changes=1),
    Scenario('navigation', 'Repeated category navigation (3 rounds over 6 categories), then one change', sc_nav,
             expect={'master_volume': '85'}, min_changes=1),
]


def second_run_script():
    s = Script()
    s.boot_to_options(); s.go_cat('audio'); s.open_page(); s.wait(300); s.finish(200)
    return s


# ---------------------------------------------------------------------------------------------------------------
# Process handling, lock, report parsing
# ---------------------------------------------------------------------------------------------------------------

def game_processes():
    try:
        out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    except OSError:
        return []
    return [l.split('","')[0].strip('"') for l in out.splitlines() if l.lower().startswith('"shadowthehedgehog')]


def take_lock(what, timeout_s):
    """New-Item semantics: fails if the file exists. Polls every 30 s."""
    deadline = time.time() + timeout_s
    while True:
        if not game_processes():
            try:
                fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, ('%s %s' % (AGENT, what)).encode('ascii', 'replace'))
                os.close(fd)
                return True
            except FileExistsError:
                pass
        if time.time() >= deadline:
            return False
        time.sleep(30)


def release_lock():
    try:
        with open(LOCK, 'r') as f:
            if not f.read().startswith(AGENT):
                return  # not ours: never delete another agent's lock
        os.remove(LOCK)
    except OSError:
        pass


def parse_cfg(data):
    out = {}
    for raw in data.decode('latin-1').splitlines():
        body = raw.split('#', 1)[0]
        if '=' not in body:
            continue
        k, v = body.split('=', 1)
        out[k.strip().lower()] = v.strip().lower()
    return out


def norm(v):
    return {'1': 'true', 'on': 'true', 'yes': 'true', '0': 'false', 'off': 'false', 'no': 'false'}.get(v, v)


def make_folder(name, cfg, display_windowed=True):
    d = os.path.join(OPTS, 'agent5-' + name)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    for f in ('card.raw', 'card.gci', 'card.gci.source', 'SRAM.pc.raw', 'pad.cfg'):
        src = os.path.join(RUN_SRC, f)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(d, f))
    with open(os.path.join(d, 'display.cfg'), 'w', newline='\n') as f:
        f.write('# agent5 lifecycle: windowed so the user\'s screen is not taken\nwidth=1280\nheight=720\n'
                'fullscreen=false\naspect=auto\nfps_counter=false\nrender_scale=67\n')
    if cfg is not None:
        with open(os.path.join(d, 'options.cfg'), 'wb') as f:
            f.write(cfg if isinstance(cfg, bytes) else cfg.encode('ascii'))
    return d


def launch(folder, script, tag, args):
    """One game launch in `folder`. Returns a dict of what was observed. The lock must be held by the caller."""
    script_path = os.path.join(folder, tag + '.uitest')
    with open(script_path, 'w', newline='\n') as f:
        f.write(script.text())
    # VI fields: 60/s NTSC, 50/s PAL; budget in game ms with the slower rate plus slack.
    budget = int(script.frames / 50.0 * 1000) + args.budget_extra_ms
    wall_limit = budget / 1000.0 * 1.6 + 240
    cmd = [EXE, '--game-data', GAME_DATA, '--memory-card', 'card.raw', '--display-cfg', 'display.cfg',
           '--pad-cfg', 'pad.cfg', '--state-file', 'unused.gekkostate', '--snapshot-key', 'none', '--no-xinput',
           '--no-audio', '--ui-test-script', script_path, '--budget-ms', str(budget)]
    res = {'tag': tag, 'script_frames': script.frames, 'budget_ms': budget, 'cmd': ' '.join(cmd)}
    if args.dry_run:
        res['dry_run'] = True
        return res
    out_p, err_p = os.path.join(folder, tag + '-stdout.log'), os.path.join(folder, tag + '-stderr.log')
    for stale in ('session-report.json',):
        try:
            os.remove(os.path.join(folder, stale))
        except OSError:
            pass
    t0 = time.time()
    with open(out_p, 'wb') as so, open(err_p, 'wb') as se:
        p = subprocess.Popen(cmd, cwd=folder, stdout=so, stderr=se)
        try:
            p.wait(timeout=wall_limit)
            res['timed_out'] = False
        except subprocess.TimeoutExpired:
            res['timed_out'] = True
            p.kill()  # our own child only
            p.wait()
    res['exit_code'] = p.returncode
    res['wall_s'] = round(time.time() - t0, 1)
    res['stdout'] = open(out_p, 'rb').read().decode('utf-8', 'replace')
    res['stderr'] = open(err_p, 'rb').read().decode('utf-8', 'replace')
    rp = os.path.join(folder, 'session-report.json')
    if os.path.exists(rp):
        try:
            res['session'] = json.load(open(rp, encoding='utf-8'))
        except ValueError:
            res['session'] = None
    return res


TILE_RE = re.compile(r'Options Display tile: (\d+) screens, (\d+) page draws, (\d+) moves, (\d+) changes, (\d+) applied '
                     r'\((\d+) refused\), (\d+) saved \((\d+) failed\)')
BAD_PATTERNS = ('did not return', 'execution fault', 'Execution fault', 'unhandled exception', 'Access violation',
                'EXCEPTION_', 'stack overflow', 'assertion failed')


def analyse(res):
    """The facts a report line / file gives; checks are in verify()."""
    out = res.get('stdout', '')
    f = {}
    m = TILE_RE.search(out)
    if m:
        keys = ('screens', 'page_draws', 'moves', 'changes', 'applied', 'refused', 'saved', 'save_failed')
        f['tile'] = dict(zip(keys, map(int, m.groups())))
    m = re.search(r'^Stop reason: (.*)$', out, re.M)
    f['stop_reason'] = m.group(1).strip() if m else None
    m = re.search(r'^Detail: (.*)$', out, re.M)
    f['stop_detail'] = m.group(1).strip()[:160] if m else None
    m = re.search(r'Save store: .*', out)
    f['save_store'] = m.group(0).strip()[:300] if m else None
    m = re.search(r'options applied (\d+), taken (\d+)', out)
    f['options_applied'] = int(m.group(1)) if m else None
    m = re.search(r'^Japanese voices: (.*)$', out, re.M)
    f['japanese_voices'] = m.group(1).strip() if m else None
    f['ui_labels'] = re.findall(r'\[ui-test\] VI (\d+): (\S+)', out)
    f['bad'] = [p for p in BAD_PATTERNS if p in out or p in res.get('stderr', '')]
    sess = res.get('session') or {}
    f['session_failed'] = (sess.get('stop') or {}).get('failed')
    f['session_reason'] = (sess.get('stop') or {}).get('reason')
    f['warnings'] = [l.strip()[:200] for l in out.splitlines() if re.search(r'options\.cfg.*(unknown key|bad value|not key)', l)]
    return f


def verify(res, facts, script, expect_labels=True):
    """(name, ok, evidence) triples."""
    checks = []
    if res.get('dry_run'):
        return checks
    checks.append(('process exited by itself within the wall limit', not res.get('timed_out'),
                   'wall %.1f s, exit %s' % (res.get('wall_s', 0), res.get('exit_code'))))
    checks.append(('Stop reason: host request (budget reached)',
                   facts['stop_reason'] is not None and 'host request' in facts['stop_reason'],
                   'Stop reason: %s | %s' % (facts['stop_reason'], facts['stop_detail'])))
    checks.append(('no execution fault / "guest call did not return" / exception', not facts['bad'] and facts['session_failed'] is not True,
                   'patterns found: %s; session stop.failed=%s' % (facts['bad'] or 'none', facts['session_failed'])))
    t = facts.get('tile')
    checks.append(('Options screen seen ("Options Display tile: N screens", N >= 1)', bool(t and t['screens'] >= 1),
                   'tile=%s' % (t,)))
    if expect_labels:
        seen = [n for _, n in facts['ui_labels']]
        missing = [l for l in script.labels if l not in seen]
        checks.append(('the script reached every label (%d)' % len(script.labels), not missing,
                       'missing: %s' % (missing[:6] or 'none')))
    return checks


# ---------------------------------------------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------------------------------------------

def run_scenario(sc, args):
    rec = {'scenario': sc.name, 'description': sc.description, 'status': 'NOT RUN', 'checks': [], 'runs': []}
    folder = make_folder(sc.name, sc.cfg)
    rec['folder'] = folder
    s1 = Script()
    sc.build(s1)
    runs = [('run1', s1, True)]
    if not args.no_restart:
        runs.append(('run2', second_run_script(), False))
    cfg_path = os.path.join(folder, 'options.cfg')
    before = open(cfg_path, 'rb').read() if os.path.exists(cfg_path) else None
    after1 = None
    for tag, script, first in runs:
        if not args.dry_run:
            if not os.path.exists(EXE):
                rec['checks'].append(('executable exists', False, EXE))
                rec['status'] = 'NOT RUN'
                return rec
            if not take_lock('%s %s' % (sc.name, tag), args.lock_timeout_s):
                rec['checks'].append(('lock obtained within %d s' % args.lock_timeout_s, False, 'RUN.lock held by: ' +
                                      (open(LOCK).read() if os.path.exists(LOCK) else '(game process running)')))
                rec['status'] = 'NOT RUN'
                return rec
        try:
            res = launch(folder, script, tag, args)
        finally:
            if not args.dry_run:
                release_lock()
        facts = analyse(res)
        res_small = {k: v for k, v in res.items() if k not in ('stdout', 'stderr', 'session')}
        res_small['facts'] = facts
        rec['runs'].append(res_small)
        if args.dry_run:
            continue
        for name, ok, ev in verify(res, facts, script):
            rec['checks'].append(('[%s] %s' % (tag, name), ok, ev))
        data = open(cfg_path, 'rb').read() if os.path.exists(cfg_path) else None
        if first:
            after1 = data
            cfg = parse_cfg(data) if data is not None else {}
            for k, want in sc.expect.items():
                got = norm(cfg.get(k, '(absent)'))
                rec['checks'].append(('[run1] options.cfg %s == %s' % (k, want), got == norm(want), 'file has %s' % got))
            if sc.name == 'rapid':
                mv = cfg.get('master_volume')
                ok = mv is None or (mv.isdigit() and 0 <= int(mv) <= 100)
                rec['checks'].append(('[run1] rapid: master_volume stays a valid 0..100 value', ok, 'file has %s' % mv))
                sp = cfg.get('speaker')
                rec['checks'].append(('[run1] rapid: speaker stays 0..2', sp is None or sp in ('0', '1', '2'), 'file has %s' % sp))
            if sc.name == 'corrupt_cfg':
                rec['checks'].append(('[run1] corrupt: the file was repaired in place (valid master_volume line)',
                                      data is not None and b'master_volume=90' in data.replace(b' ', b''), 'ok' if data else 'no file'))
                rec['checks'].append(('[run1] corrupt: game started with defaults + warnings',
                                      bool(facts['warnings']), 'warnings: %s' % (facts['warnings'][:3] or 'none')))
            if sc.min_changes:
                ch = facts.get('tile', {}).get('changes', 0)
                rec['checks'].append(('[run1] the menu counted >= %d change(s)' % sc.min_changes, ch >= sc.min_changes,
                                      'changes=%s applied=%s saved=%s' % (ch, facts.get('tile', {}).get('applied'),
                                                                          facts.get('tile', {}).get('saved'))))
            if sc.cfg is None and sc.name == 'fresh_boot':
                rec['checks'].append(('[run1] fresh boot: no options.cfg was required (file now %s)' % ('present' if data else 'absent'),
                                      True, 'informational'))
        else:
            rec['checks'].append(('[run2] options.cfg unchanged by a restart with no edits',
                                  data == after1, 'bytes equal' if data == after1 else 'DIFFERENT'))
            cfg = parse_cfg(data) if data is not None else {}
            for k, want in sc.expect.items():
                got = norm(cfg.get(k, '(absent)'))
                rec['checks'].append(('[run2] restored: options.cfg %s == %s' % (k, want), got == norm(want), 'file has %s' % got))
            if data and any(k in cfg for k in ('language', 'speaker', 'vibration', 'voice', 'autosave')):
                oa = facts.get('options_applied')
                rec['checks'].append(('[run2] boot applied options.cfg to the game ("options applied" >= 1 on the Save store line)',
                                      oa is not None and oa >= 1, 'save store: %s' % facts.get('save_store')))
    if args.dry_run:
        rec['status'] = 'DRY RUN'
    else:
        rec['status'] = 'PASS' if rec['checks'] and all(ok for _, ok, _ in rec['checks']) else 'FAIL'
    return rec


def main():
    global EXE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scenarios', nargs='*')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--no-restart', action='store_true')
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--budget-extra-ms', type=int, default=20000)
    ap.add_argument('--lock-timeout-s', type=int, default=1200)
    ap.add_argument('--out-json', default=os.path.join(OPTS, 'agent5-lifecycle-results.json'))
    args = ap.parse_args()
    EXE = args.exe
    if args.list:
        for sc in SCENARIOS:
            print('%-18s %s' % (sc.name, sc.description))
        return 0
    chosen = SCENARIOS if args.all else [sc for sc in SCENARIOS if sc.name in args.scenarios]
    unknown = [n for n in args.scenarios if n not in [sc.name for sc in SCENARIOS]]
    if unknown or not chosen:
        print('no such scenario: %s (see --list)' % (unknown or 'none selected'))
        return 2
    results = []
    for sc in chosen:
        print('== %s: %s' % (sc.name, sc.description), flush=True)
        rec = run_scenario(sc, args)
        results.append(rec)
        for name, ok, ev in rec['checks']:
            print('   [%s] %s -- %s' % ('ok' if ok else 'FAIL', name, ev))
        print('   => %s' % rec['status'], flush=True)
        with open(args.out_json, 'w', encoding='utf-8') as f:
            json.dump(results, f, indent=1, default=str)
    print('\nSummary: ' + ', '.join('%s=%s' % (r['scenario'], r['status']) for r in results))
    return 0 if all(r['status'] in ('PASS', 'DRY RUN') for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
