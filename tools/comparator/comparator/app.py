"""The workstation's model: configuration, the snapshot store, the two sides' sessions, materialisation, thumbnails,
the lockstep runs, the one-button snapshots and the frame recreations. The UI (ui.py) only presents this.

Time. The game clock (game ticks, 40.5 MHz; a host service every 5 ms of it) is the one clock both builds share
exactly. Their frame counters are their own (the legacy capture counts presents, the lifted driver its submitted
frames - one apart from a process's start), so everything that must agree between the two sides is keyed by game
time: a lockstep run advances both by the same game time and pauses each at the first traversal boundary after it,
and a lockstep capture is the first frame each build finishes at or after the same game tick (both report the
frame's tick, which must be equal).

One-button snapshots (docs/comparator/README.md "One-button snapshots"). The snapshot key in a game window (F5 by
default; Create Snapshot asks the same through the link) makes that build save, at its next simulation boundary, its
own savestate, the input tape from its start, a state report and the next frame into a folder of its session; the
workstation adopts it into the store at once - named and described from what the savestate says the moment is
(semantics.py: stage, mission, checkpoint, player, camera, live objects, what is happening) - and then, in the
background, brings the other build to the same moment by replaying the tape, saves both builds' warm-up states and
recreates the frame in both (recreate.py) for a pixel comparison.
"""
from __future__ import annotations

import datetime
import json
import re
import shutil
import subprocess
import threading
import uuid
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from . import config as config_mod
from . import imagediff, probes, semantics
from .checkpoints import CheckpointStore, now_iso, valid_name
from .devlink import DevLinkServer
from .recreate import RecreateJob, compare_files
from .sessions import (FIELD_TICKS, TICKS_PER_SECOND, GameSession, HeadlessRun, cleanup_sessions, file_sha256,
                       read_state_header, snapshot_save_data)
from .snapshots import THUMB_SIZE, ThumbnailJob, summary
from .statefile import GuestImage, StateFileError

SIDES = ('before', 'after')
STAGE_RE = re.compile(r'^\s*(stg\d+)\s+(\S+)\s+(bootable|not bootable)(.*)$')
LIVE_PEEKS = ','.join(p.range for p in probes.PROBES if p.name in (
    'stage.number', 'game.frame_counter', 'game.time_remaining', 'player1.rings', 'player1.score', 'camera.eye',
    'camera.hfov', 'player1.class', 'player1.position'))


def other_side(side: str) -> str:
    return 'after' if side == 'before' else 'before'


def idle_tape(path: Path, start_tick: int, width: int, height: int):
    """A tape that latches an idle, connected controller on port 0 (the others absent) from `start_tick`: the input of
    a lockstep run, so neither window's live input reaches its build."""
    lines = ['shadow-input-tape 1', f'start {start_tick}', f'display {width}x{height}']
    for port in range(4):
        lines.append(f"sample {start_tick} {port} {1 if port == 0 else 0} 0000 0 0 0 0")
    lines.append('end')
    Path(path).write_text('\n'.join(lines) + '\n', encoding='utf-8')


class Comparator(QObject):
    message = Signal(str)            # the tool's log
    sessions_changed = Signal()
    checkpoints_changed = Signal()
    materialised = Signal(str, str, bool, str)   # snapshot, profile, ok, text
    captures_ready = Signal(dict)    # {'before': path, 'after': path, 'label': ..., 'stats': ...}
    states_ready = Signal(dict)      # {'before': report, 'after': report}
    snapshot_created = Signal(str, str)          # name, side (a one-button snapshot, adopted into the store)
    recreated = Signal(dict)         # a recreation finished: {'checkpoint', 'renders': {profile: result}, ...}
    stages_ready = Signal(list)
    live_info = Signal(str, dict)    # side, {probe values, clocks}

    def __init__(self, config_path: Path = config_mod.CONFIG_PATH):
        super().__init__()
        self.config_path = config_path
        self.config = config_mod.load_config(config_path)
        self.store = CheckpointStore(Path(self.config['states_dir']))
        self.store.ensure_seed(self.seed_candidates())
        cleanup_sessions(self.store.sessions_dir, float(self.config.get('keep_sessions_days', 3)))
        self.devlink = DevLinkServer(self)
        self.devlink.connection_ready.connect(self._connection_ready)
        self.sessions = {'before': None, 'after': None}
        self._by_token = {}
        self._jobs = []
        self._thumbs = []
        self._recreations = []          # RecreateJob
        self._recreate_sets = {}        # checkpoint name -> {'want': set, 'results': {}, 'label', 'auto'}
        self._recreate_after = {}       # checkpoint name -> (sides, label): asked while it was being made
        self._live_folders = {}         # a game's snapshot folder -> the snapshot it became
        self._snapshot_requests = []    # (side, name, description) asked through Create Snapshot / New...
        self._saving = set()            # snapshots whose live save has not been answered yet
        self._pending_renames = {}      # name -> (new name, description), applied when the snapshot is idle
        self._store_lock = threading.RLock()
        self.lockstep = None     # {'checkpoint': name, 'ticks': game ticks advanced} while both advance together
        self.running_lockstep = False
        self.stages = []

    # ---- configuration ----
    def save_config(self):
        config_mod.save_config(self.config, self.config_path)

    def profile_of(self, side: str) -> str:
        return self.config['before'] if side == 'before' else self.config['after']

    def side_of(self, profile: str):
        for side in SIDES:
            if self.profile_of(side) == profile:
                return side
        return None

    def side_label(self, side: str) -> str:
        return self.profile_of(side).upper()

    def seed_candidates(self):
        return [self.config.get('seed_save_data') or '', Path(self.config['game_data']) / 'save-data']

    @property
    def hold(self) -> bool:
        """Launches and loads stop on their first frame (both sides in lockstep) instead of running free."""
        return bool(self.config['window'].get('hold', True))

    @property
    def snapshot_key(self) -> str:
        return self.config.get('snapshot_hotkey') or 'F5'

    def load_stages(self):
        """The stage table from the AFTER build's --list-stages (as the stage launcher does)."""
        exe = Path(self.config['profiles'][self.profile_of('after')]['exe'])

        def work():
            stages = []
            try:
                out = subprocess.run([str(exe), '--game-data', self.config['game_data'], '--list-stages'],
                                     capture_output=True, timeout=120,
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout
                for raw in out.decode('utf-8', errors='replace').splitlines():
                    m = STAGE_RE.match(raw)
                    if m:
                        stages.append((m.group(1), semantics.stage_title(m.group(1)) or m.group(2),
                                       m.group(3) == 'bootable'))
            except (OSError, subprocess.SubprocessError):
                pass
            self.stages = stages
            self.stages_ready.emit(stages)

        threading.Thread(target=work, daemon=True).start()

    def stage_name(self, stage: str) -> str:
        if stage in semantics.STAGE_TITLES:
            return semantics.STAGE_TITLES[stage]
        for stg, name, _b in self.stages:
            if stg == stage:
                return name
        return ''

    # ---- sessions ----
    def register_session(self, session: GameSession):
        self._by_token[session.token] = session

    def connected(self, side) -> bool:
        s = self.sessions.get(side)
        return s is not None and s.connection is not None

    def _connection_ready(self, connection):
        token = (connection.info or {}).get('name')
        session = self._by_token.get(token)
        if session is None:
            self.message.emit(f'a game connected with an unknown token {token!r}; closing it')
            connection.close()
            return
        session.attach(connection)
        connection.event.connect(lambda m, s=session: self._session_event(s, m))
        self.sessions_changed.emit()
        self._started(session)

    def _session_event(self, session, message):
        event = message.get('event')
        try:
            if event == 'snapshot':
                self._adopt_live_snapshot(session, message)
            elif event == 'snapshot_reference':
                self._adopt_reference(session, message)
        except Exception as error:  # noqa: BLE001 - a snapshot must never take the workstation down
            self.message.emit(f'[Snapshot] adopting the snapshot failed: {error}')

    def launch(self, side: str, start: dict, together: bool = False, run_free: bool = False):
        """A new game on `side` (a running one there is stopped). `run_free`: it starts running (playing from a
        route), whatever the hold setting."""
        old = self.sessions.get(side)
        if old is not None and old.running():
            old.stop()
        session = GameSession(self, side, self.profile_of(side), start)
        session.status_changed.connect(self.sessions_changed)
        session.finished.connect(lambda _code: self.sessions_changed.emit())
        session.together = together
        session.run_free = run_free
        self.sessions[side] = session
        self.lockstep = None
        session.launch()
        self.sessions_changed.emit()
        return session

    def play(self, side: str, start: dict):
        """PLAY: a build windowed, running free with live input, ready for the snapshot key."""
        return self.launch(side, start, run_free=True)

    def _started(self, session):
        """A launched game has connected (paused before its first instruction). A "load both" waits for the other
        side; then both run free - or, holding, both run one field and stay paused in lockstep."""
        if getattr(session, 'together', False):
            partner = self.sessions[other_side(session.side)]
            if partner is None or partner.connection is None or getattr(partner, 'pending_load', False):
                return
            if getattr(partner, 'together', False) or getattr(partner, 'loaded_for_pair', False):
                self._pair_ready(session.start_point.get('checkpoint'))
            return
        if self.hold and not getattr(session, 'run_free', False):
            self.run_lockstep([session.side], FIELD_TICKS, capture=False)
        else:
            session.request('resume')

    def _pair_ready(self, cp):
        for s in SIDES:
            self.sessions[s].together = False
            self.sessions[s].loaded_for_pair = False
        # The lockstep is declared by the hold run's answer (run_lockstep), not before it.
        self.lockstep = {'checkpoint': cp.name, 'ticks': 0} if cp is not None else None
        if self.hold:
            self.run_lockstep(SIDES, FIELD_TICKS, capture=False, what='hold')
        else:
            for s in SIDES:
                self.resume(s)
        self.sessions_changed.emit()

    # ---- loading a snapshot ----
    def load(self, side: str, cp, pair: bool = False):
        """The snapshot in this side's build: in the running game when there is one (fast: its own savestate,
        restored at the next traversal boundary), else in a newly launched one."""
        profile = self.profile_of(side)
        if not cp.has_state(profile):
            raise RuntimeError(f"'{cp.name}' has no {profile} state yet (it is made in the background)")
        session = self.sessions.get(side)
        if session is None or session.connection is None or session.profile_id != profile:
            session = self.launch(side, {'kind': 'checkpoint', 'checkpoint': cp}, together=pair)
            return session
        if getattr(session, 'run_in_flight', False):
            raise RuntimeError(f'{self.side_label(side)}: a run is in progress; load when it has paused')
        quick = session.folder / 'quick.gekkostate'
        shutil.copy2(cp.state_file(profile), quick)
        session.pending_load = True
        self.lockstep = None

        def loaded(reply):
            session.pending_load = False
            if not reply.get('ok'):
                self.message.emit(f"{self.side_label(side)}: loading '{cp.name}' failed: {reply.get('error')}")
                return
            session.start_point = {'kind': 'checkpoint', 'checkpoint': cp}
            session.root_state = quick
            session.root_valid = True
            self.message.emit(f"{self.side_label(side)}: '{cp.name}' loaded in the running game")
            if pair:
                session.loaded_for_pair = True
                partner = self.sessions[other_side(side)]
                if partner is not None and partner.connection is not None and \
                        not getattr(partner, 'pending_load', False) and \
                        (getattr(partner, 'loaded_for_pair', False) or getattr(partner, 'together', False) and
                         partner.paused):
                    self._pair_ready(cp)
            elif self.hold:
                self.run_lockstep([side], FIELD_TICKS, capture=False)
            else:
                session.request('resume')
            self.sessions_changed.emit()

        session.request('load', loaded, path=str(quick), pause=True)
        return session

    def load_both(self, cp):
        missing = [self.profile_of(s) for s in SIDES if not cp.has_state(self.profile_of(s))]
        if missing:
            raise RuntimeError(f"'{cp.name}' has no state yet for {', '.join(missing)}")
        for s in SIDES:
            self.load(s, cp, pair=True)

    def reload(self, side: str):
        session = self.sessions.get(side)
        if session is None or session.connection is None:
            self.message.emit(f'{self.side_label(side)}: no connected game')
            return
        if session.start_point.get('kind') != 'checkpoint':
            self.message.emit(f'{self.side_label(side)}: this game started from a boot route; launch it again')
            return
        self.load(side, session.start_point['checkpoint'])

    def reload_both(self):
        cps = [self.sessions[s].start_point.get('checkpoint') if self.connected(s) else None for s in SIDES]
        if not all(cps):
            self.message.emit('reload both: both sides need a game loaded from a snapshot')
            return
        if cps[0].name != cps[1].name:
            self.message.emit('reload both: the two sides were loaded from different snapshots; Load BOTH instead')
            return
        self.load_both(cps[0])

    def pause(self, side):
        s = self.sessions.get(side)
        if s is not None:
            s.request('pause')
            self.lockstep = None

    def resume(self, side):
        """Runs free again: a lockstep run's idle input tape stops, the window's live input is latched again."""
        s = self.sessions.get(side)
        if s is not None:
            s.request('tape_stop')
            s.request('resume')
            self.lockstep = None

    def stop(self, side):
        s = self.sessions.get(side)
        if s is not None:
            s.stop()

    # ---- stepping and captures ----
    def capture_dir(self) -> Path:
        cp = self.store.get(self.lockstep['checkpoint']) if self.lockstep else None
        folder = (cp.path / 'captures') if cp else (self.store.root / '.captures')
        folder.mkdir(parents=True, exist_ok=True)
        return folder

    def run_lockstep(self, sides, ticks: int, capture: bool, what: str = ''):
        """Each side advances by `ticks` of game time and pauses at the next traversal boundary. In lockstep both play
        the same idle input tape, so neither window's live input reaches its build.

        A capture is the frame of the run's second-to-last field, keyed by its game tick: in every frame cycle the
        lifted renderer receives its frame before the traversal boundary (the display copy) and the legacy one after
        it (the end of RwCameraShowRaster), both stamped with the cycle's tick - so at a pause the legacy build has
        not presented the last cycle's frame yet, and the cycle before it is the one both builds have (measured:
        after a load the legacy build presents the load tick's frame, the lifted one starts one field later). A
        capture run is therefore at least two fields long."""
        sides = list(sides)
        if capture:
            ticks = max(int(ticks), 2 * FIELD_TICKS)
        sessions = [self.sessions[s] for s in sides]
        if not all(s is not None and s.connection is not None for s in sessions):
            self.message.emit('run: a side has no connected game')
            return
        if any(getattr(s, 'run_in_flight', False) or getattr(s, 'pending_load', False) for s in sessions):
            self.message.emit('run: a run or a load is still in progress')
            return
        if not all(s.paused for s in sessions):
            self.message.emit('run: pause first (both run from where they are paused)')
            return
        for s in sessions:
            s.run_in_flight = True   # until its answer: no second run can start from a stale "paused"
        starts = {s.side: int(s.last_status.get('game_ticks', 0) or 0) for s in sessions}
        lockstep_before = dict(self.lockstep) if (self.lockstep and len(sides) == 2) else None
        if len(sides) == 2 and starts['before'] != starts['after']:
            self.message.emit(f"run: the sides are at different game ticks ({starts['before']} vs {starts['after']}):"
                              ' not in lockstep - Load BOTH first for an exact comparison')
            lockstep_before = None
        stamp_text = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        folder = self.capture_dir()
        advanced = (lockstep_before['ticks'] if lockstep_before else 0) + ticks
        # No '.' in the label: it becomes part of the capture file names.
        label = (f"{lockstep_before['checkpoint']}+{advanced}t" if lockstep_before
                 else f'{"-".join(sides)}-free')
        win = self.config['window']
        results = {}
        self.running_lockstep = True

        def finished(side, reply):
            results[side] = reply
            self.sessions[side].run_in_flight = False
            if len(results) < len(sessions):
                return
            self.running_lockstep = False
            ok = all(r.get('ok') for r in results.values())
            if lockstep_before is not None and ok:
                self.lockstep = {'checkpoint': lockstep_before['checkpoint'], 'ticks': advanced}
            elif len(sides) == 2 or not ok:
                self.lockstep = None
            payload = {'label': label, 'ticks': ticks, 'what': what}
            for s, r in results.items():
                payload[s] = r.get('capture') if r.get('captured') else None
                payload[s + '_reply'] = r
            if capture and len(sides) == 2:
                ticks_seen = {s: results[s].get('capture_game_ticks') for s in sides}
                payload['same_frame'] = ticks_seen['before'] == ticks_seen['after'] and ticks_seen['before']
                payload['frame_ticks'] = ticks_seen
                if payload.get('before') and payload.get('after'):
                    self._record_comparison(payload)
            self.sessions_changed.emit()
            if capture:
                self.captures_ready.emit(payload)
            self.request_states(sides)
            self.poll_live()   # the CURRENT GAME panels at once (polls wait while a run is in flight)

        for s in sessions:
            start = starts[s.side]
            capture_tick = start + ticks - FIELD_TICKS

            def go(_reply=None, s=s, capture_tick=capture_tick):
                # The game sets the extension (PNG / PPM): the stem must not contain a '.'.
                stem = re.sub(r'[^A-Za-z0-9_+-]', '_', f'{stamp_text}-{label}-{self.profile_of(s.side)}')
                path = str(folder / stem) if capture else None
                s.request('run_ticks', lambda r, side=s.side: finished(side, r), ticks=ticks, path=path,
                          capture_tick=capture_tick if capture else None, width=win['width'], height=win['height'])

            if lockstep_before is not None or len(sides) == 1:
                tape = s.folder / f'idle-{stamp_text}.tape'
                idle_tape(tape, start, win['width'], win['height'])
                s.request('tape_play', go, path=str(tape))
            else:
                go()

    def step_tick(self, sides):
        """To the next traversal boundary (the game's next simulation step), at least one game tick on."""
        self.run_lockstep(sides, 1, capture=False, what='step tick')

    def _record_comparison(self, payload):
        if not self.lockstep:
            return
        try:
            a, b = imagediff.pair(imagediff.load(payload['before']), imagediff.load(payload['after']),
                                  bool(self.config['capture'].get('fit_before_to_after', True)))
            _image, stats = imagediff.difference(a, b, int(self.config['capture'].get('diff_threshold', 8)))
        except (OSError, ValueError):
            return
        payload['stats'] = stats
        with self._store_lock:
            cp = self.store.get(self.lockstep['checkpoint'])
            if cp is None:
                return
            cp.meta['last_comparison'] = {
                'when': now_iso(), 'label': payload['label'], 'after_ticks': self.lockstep['ticks'],
                'same_frame': bool(payload.get('same_frame')), 'frame_ticks': payload.get('frame_ticks'),
                'before': payload['before'], 'after': payload['after'], 'stats': stats, 'kind': 'lockstep',
            }
            cp.save()
        self.checkpoints_changed.emit()

    def request_states(self, sides=SIDES, memory=True):
        sessions = {s: self.sessions[s] for s in sides if self.connected(s)}
        reports = {}

        def got(side, reply):
            reports[side] = reply.get('report') if reply.get('ok') else None
            if len(reports) == len(sessions):
                self.states_ready.emit(reports)

        for side, s in sessions.items():
            if not s.paused:
                reports[side] = None
                continue
            s.request('report', lambda r, side=side: got(side, r), peeks=probes.peek_list(), memory=memory)
        if sessions and len(reports) == len(sessions):
            self.states_ready.emit(reports)

    def poll_live(self):
        """The CURRENT GAME readout: a small report from each connected game (not during a lockstep run)."""
        if self.running_lockstep:
            return
        for side in SIDES:
            s = self.sessions.get(side)
            if s is None or s.connection is None or getattr(s, 'pending_load', False):
                continue

            # `s` bound here: the answer comes after the loop has moved on (to a side that may have no game).
            def got(reply, side=side, s=s):
                if reply.get('ok') and reply.get('report'):
                    info = summary(reply['report'], self.stages)
                    info['paused'] = s.paused
                    self.live_info.emit(side, info)

            s.request('report', got, peeks=LIVE_PEEKS, memory=False)

    # ---- one-button snapshots ----
    def create_snapshot(self, side: str, name: str = None, description: str = ''):
        """The snapshot key's action in the game on `side` (Create Snapshot, New... from a running game): the build
        takes the snapshot at the boundary the request is served at; it is adopted when its "snapshot" event comes
        (named from its semantics unless `name` is given)."""
        session = self.sessions.get(side)
        if session is None or session.connection is None:
            raise RuntimeError(f'{self.side_label(side)}: no connected game')
        if name and self.store.get(name) is not None:
            raise ValueError(f"a snapshot named '{name}' exists")
        self._snapshot_requests.append((side, name, description))
        self.message.emit(f'[Snapshot] asking {self.side_label(side)} for a snapshot of this moment...')

        def answered(reply):
            if not reply.get('ok'):
                self._snapshot_requests = [r for r in self._snapshot_requests if r[0] != side]
                self.message.emit(f"[Snapshot] {self.side_label(side)} refused: {reply.get('error')}")

        session.request('snapshot', answered)

    def _requested_name(self, side):
        for i, (s, name, description) in enumerate(self._snapshot_requests):
            if s == side:
                del self._snapshot_requests[i]
                return name, description
        return None, ''

    def _session_root(self, session):
        start = session.start_point
        if start['kind'] == 'checkpoint':
            return {'kind': 'checkpoint', 'name': start['checkpoint'].name}
        return {k: v for k, v in start.items() if k in ('kind', 'stage')}

    def _unique_name(self, name: str) -> str:
        base = name[:92] or 'snapshot'
        candidate = base
        n = 2
        while self.store.get(candidate) is not None or not valid_name(candidate):
            candidate = re.sub(r'[^A-Za-z0-9_.-]', '', f'{base}-{n}') or f'snapshot-{n}'
            n += 1
        return candidate

    def _adopt_live_snapshot(self, session, message):
        """The game saved a snapshot into its session folder: into the store now, named from its semantics; the
        other build and the warm-up states follow in the background."""
        folder = Path(message.get('folder') or '')
        manifest = message.get('manifest') or {}
        if not message.get('ok') or not (folder / 'state.gekkostate').exists():
            self.message.emit(f"[Snapshot] {self.side_label(session.side)}: the snapshot failed: "
                              f"{manifest.get('error', 'no state saved')}")
            return
        profile = session.profile_id
        image = GuestImage(folder / 'state.gekkostate')
        sem = semantics.extract(image, self.stages)
        when = datetime.datetime.now()
        unique = uuid.uuid4().hex[:8]
        lab = semantics.labels(sem, when, unique)
        wanted, description = self._requested_name(session.side)
        name = self._unique_name(wanted or lab['name'])
        ticks = int(manifest.get('game_ticks') or image.game_ticks)
        boundary = manifest.get('boundary_request_tick') or None
        width = int(manifest.get('width') or self.config['window']['width'])
        height = int(manifest.get('height') or self.config['window']['height'])
        tape_ok = bool(manifest.get('tape')) and (folder / 'input.tape').exists() and session.root_valid
        root = self._session_root(session)
        recipe = {'root': root, 'tape': 'input.tape' if tape_ok else None, 'ui_test_script': None,
                  'stop_tick': boundary, 'display': [width, height], 'frame_tick': ticks + 1,
                  'warmup_tick': self._warmup_tick(ticks, manifest.get('tape_start_tick'))}
        with self._store_lock:
            cp = self.store.create(name, recipe, description)
            if session.start_point.get('ui_test_script'):
                shutil.copy2(session.start_point['ui_test_script'], cp.path / 'route.uitest')
                cp.recipe['ui_test_script'] = 'route.uitest'
            cp.state_file(profile).parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(folder / 'state.gekkostate'), cp.state_file(profile))
            if (folder / 'report.json').exists():
                shutil.copy2(folder / 'report.json', cp.report_file(profile))
            if tape_ok:
                shutil.copy2(folder / 'input.tape', cp.path / 'input.tape')
            shutil.copy2(folder / 'snapshot.json', cp.path / 'snapshot.json')
            snapshot_save_data(session.folder, cp.save_data_dir(profile))
            cp.meta.update({'kind': 'one-button', 'title': lab['title'], 'auto_name': lab['name'],
                            'snapshot_id': unique, 'semantics': sem, 'saved_tick': ticks,
                            'live': {k: v for k, v in manifest.items()},
                            'made_from': {'side': session.side, 'profile': profile, 'session': str(session.folder),
                                          'root_valid': session.root_valid, 'start': session.start_label()}})
            self._record_state(cp, profile, 'live', Path(session.profile['exe']), ticks, boundary)
            cp.save()
        self._live_folders[str(folder).lower()] = cp.name
        for line in semantics.log_lines(sem, cp.name, f'{self.side_label(session.side)}, '
                                        f"{manifest.get('flavour')} build, tick {ticks}"):
            self.message.emit(line)
        if not tape_ok:
            self.message.emit(f"[Snapshot] '{cp.name}' exists for {profile} only: "
                              + ("the game's start was replaced by an M quick-load" if not session.root_valid
                                 else manifest.get('tape_error') or 'no input tape'))
        reference = message.get('reference')
        if reference:
            self._adopt_reference(session, {'folder': str(folder), 'reference': reference})
        self.checkpoints_changed.emit()
        self.snapshot_created.emit(cp.name, session.side)
        self.complete_snapshot(cp)

    def _warmup_tick(self, ticks: int, root_tick):
        fields = int((self.config.get('recreate') or {}).get('warmup_fields', 60))
        if fields <= 0:
            return None
        warm = int(ticks) - fields * FIELD_TICKS
        start = int(root_tick or 0)
        return warm if warm > start + FIELD_TICKS else None

    def _adopt_reference(self, session, message):
        """The frame the game showed right after its snapshot's boundary: the snapshot's reference picture (and
        thumbnail); its tick is the frame a recreation renders."""
        folder = str(Path(message.get('folder') or '')).lower()
        name = self._live_folders.get(folder)
        reference = message.get('reference') or {}
        if name is None or not reference.get('path'):
            return
        with self._store_lock:
            cp = self.store.get(name)
            if cp is None:
                return
            source = Path(reference['path'])
            if not source.exists():
                self.message.emit(f"[Snapshot] '{name}': the reference frame {source} is missing")
                return
            image = imagediff.load(source)
            profile = session.profile_id
            target = cp.path / 'reference' / f'live-{profile}.png'
            target.parent.mkdir(exist_ok=True)
            imagediff.save_png(image, target)
            width, height = cp.recipe.get('display') or (image.shape[1], image.shape[0])
            shown = imagediff.fit(image, int(width), int(height)) if image.shape[1] != int(width) else image
            imagediff.save_png(shown, cp.path / 'first-frame.png')
            imagediff.save_png(imagediff.fit(shown, *THUMB_SIZE), cp.path / 'thumb.png')
            cp.meta['reference'] = {'file': f'reference/live-{profile}.png', 'profile': profile,
                                    'frame': reference.get('frame'),
                                    'frame_game_ticks': reference.get('frame_game_ticks'),
                                    'size': [int(image.shape[1]), int(image.shape[0])]}
            if reference.get('frame_game_ticks'):
                cp.recipe['frame_tick'] = int(reference['frame_game_ticks'])
            cp.save()
        self.message.emit(f"[Snapshot] '{name}': reference frame {reference.get('frame')} (game tick "
                          f"{reference.get('frame_game_ticks')}) from {self.side_label(session.side)}")
        self.checkpoints_changed.emit()

    def complete_snapshot(self, cp):
        """What a snapshot still needs for LOAD BOTH and RECREATE EXACT FRAME: every build's state (headless
        replay) and every build's warm-up state; then (setting recreate.auto) its recreation."""
        if cp.meta.get('kind') == 'one-button' and not cp.recipe.get('tape'):
            return   # no input to replay: it exists for the build that made it only
        for profile in dict.fromkeys(self.profile_of(s) for s in SIDES):
            if self._job_for(cp.name, profile):
                continue
            if not cp.has_state(profile):
                self.materialise(cp, profile)
            elif cp.recipe.get('warmup_tick') and not cp.has_warmup(profile):
                self.make_warmup(cp, profile)
        self._maybe_auto_recreate(cp.name)

    def _job_for(self, name, profile):
        return any(j.cp.name == name and j.profile_id == profile for j in self._jobs)

    def make_warmup(self, cp, profile):
        """The warm-up state of a build that has the snapshot's own state (the live one): a replay to the snapshot's
        boundary saving it on the way - and the replayed end state's report, which must equal the live save's."""
        if not cp.recipe.get('warmup_tick'):
            return
        root = cp.recipe.get('root', {})
        if root.get('kind') == 'checkpoint':
            parent = self.store.get(root.get('name'))
            if parent is None or not parent.has_state(profile):
                self.message.emit(f"'{cp.name}': no warm-up for {profile} (its root '{root.get('name')}' is missing)")
                return
        job = HeadlessRun(self, cp, profile, purpose='warmup')
        job.done.connect(lambda ok, text, job=job: self._job_done(job, ok, text, None))
        self._jobs.append(job)
        self.message.emit(f"'{cp.name}': the warm-up state for {profile} (headless replay)...")
        self.sessions_changed.emit()
        job.start()

    def busy(self, name: str) -> bool:
        """A snapshot whose live save, replays, thumbnail or recreation are still being made (its folder must not
        move)."""
        return name in self._saving or any(j.cp.name == name for j in self._jobs) or \
            any(j.cp.name == name for j in self._thumbs) or any(j.cp.name == name for j in self._recreations)

    def rename(self, old: str, new: str, description: str = None, title: str = None):
        """Renames now, or as soon as the snapshot's jobs are done (a fresh snapshot is still being made)."""
        if self.busy(old):
            self._pending_renames[old] = (new, description, title)
            self.message.emit(f"'{old}' will be renamed '{new}' once it is complete")
            return None
        return self._rename_now(old, new, description, title)

    def _apply_pending_rename(self, name: str):
        """Applies a rename that waited for the snapshot's jobs; returns the snapshot's name now."""
        if name in self._pending_renames and not self.busy(name):
            new, description, title = self._pending_renames.pop(name)
            try:
                cp = self._rename_now(name, new, description, title)
                return cp.name if cp is not None else name
            except Exception as error:  # noqa: BLE001 - reported
                self.message.emit(f"renaming '{name}' to '{new}' failed: {error}")
        return name

    def _rename_now(self, old: str, new: str, description: str = None, title: str = None):
        with self._store_lock:
            if self.store.get(old) is None:
                # Renamed already (a naming dialog answered after another rename): the description still applies.
                cp = self.store.get(new)
                if cp is None:
                    self.message.emit(f"rename: no snapshot '{old}'")
                    return None
                old = new
            cp = self.store.rename(old, new) if new != old else self.store.get(old)
            if description is not None:
                cp.meta['description'] = description
            if title is not None and title != cp.meta.get('title'):
                cp.meta['title'] = title
                cp.meta['title_user'] = True     # Re-read semantics keeps it
            cp.save()
        for s in SIDES:
            session = self.sessions.get(s)
            if session is not None and session.start_point.get('kind') == 'checkpoint' and \
                    session.start_point['checkpoint'].name == old:
                session.start_point = {'kind': 'checkpoint', 'checkpoint': cp}
        for key, value in list(self._live_folders.items()):
            if value == old:
                self._live_folders[key] = new
        if self.lockstep and self.lockstep['checkpoint'] == old:
            self.lockstep['checkpoint'] = new
        self.checkpoints_changed.emit()
        return cp

    def new_checkpoint_from_route(self, name: str, route: dict, seconds: float, description: str = ''):
        width, height = self.config['window']['width'], self.config['window']['height']
        root = {k: v for k, v in route.items() if k in ('kind', 'stage')}
        stop = int(seconds * TICKS_PER_SECOND)
        recipe = {'root': root, 'tape': None, 'ui_test_script': None, 'stop_tick': stop, 'display': [width, height],
                  'warmup_tick': self._warmup_tick(stop, 0)}
        with self._store_lock:
            cp = self.store.create(name, recipe, description)
            if route.get('ui_test_script'):
                shutil.copy2(route['ui_test_script'], cp.path / 'route.uitest')
                cp.recipe['ui_test_script'] = 'route.uitest'
                cp.save()
        self.checkpoints_changed.emit()
        self.materialise_all(cp)
        return cp

    def derive_checkpoint(self, parent_name: str, name: str, seconds: float, description: str = ''):
        parent = self.store.get(parent_name)
        if parent is None or parent.saved_tick is None:
            raise ValueError(f"'{parent_name}' has no state yet")
        width, height = parent.recipe.get('display') or (self.config['window']['width'],
                                                           self.config['window']['height'])
        stop = int(parent.saved_tick + seconds * TICKS_PER_SECOND)
        recipe = {'root': {'kind': 'checkpoint', 'name': parent_name}, 'tape': 'input.tape', 'ui_test_script': None,
                  'stop_tick': stop, 'display': [width, height], 'warmup_tick': self._warmup_tick(stop,
                                                                                                  parent.saved_tick)}
        with self._store_lock:
            cp = self.store.create(name, recipe, description or f'{parent_name} + {seconds:g} s, idle input')
            idle_tape(cp.path / 'input.tape', parent.saved_tick, width, height)
        self.checkpoints_changed.emit()
        self.materialise_all(cp)
        return cp

    def _record_state(self, cp, profile, method, exe: Path, game_ticks, boundary_tick):
        header = read_state_header(cp.state_file(profile))
        previous = cp.states.get(profile) or {}
        cp.states[profile] = {
            'file': f'states/{profile}.gekkostate', 'report': f'states/{profile}.report.json',
            'save_data': f'states/{profile}.save-data', 'method': method, 'created': now_iso(),
            'exe': str(exe), 'exe_sha256': file_sha256(exe), 'game_ticks': game_ticks,
            'boundary_tick': boundary_tick, 'savestate_version': header[0] if header else None,
            'renderer_kind': header[1] if header else None,
        }
        for key in ('warmup', 'replay_check'):
            if key in previous:
                cp.states[profile][key] = previous[key]
        report = cp.report(profile)
        if report and not cp.meta.get('summary'):
            cp.meta['summary'] = summary(report, self.stages)
        if not cp.meta.get('semantics'):
            self._describe(cp, profile)
        self.verify(cp)

    def _describe(self, cp, profile):
        """Semantics and a readable title for a snapshot that has none yet (route and derived ones)."""
        try:
            sem = semantics.extract(GuestImage(cp.state_file(profile)), self.stages)
        except (OSError, StateFileError) as error:
            self.message.emit(f"'{cp.name}': its state could not be read ({error})")
            return
        cp.meta['semantics'] = sem
        if not cp.meta.get('title'):
            cp.meta['title'] = semantics.labels(sem, datetime.datetime.now(), '')['title']
        cp.meta.setdefault('snapshot_id', uuid.uuid4().hex[:8])

    def redescribe(self, cp):
        """Reads the snapshot's semantics again (a better reader applies to every snapshot already made)."""
        with self._store_lock:
            fresh = self.store.get(cp.name) or cp
            profile = next((p for p in fresh.states if fresh.has_state(p)), None)
            if profile is None:
                return
            fresh.meta.pop('semantics', None)
            keep_title = fresh.meta.get('title_user') or (fresh.meta.get('kind') != 'one-button' and
                                                          fresh.meta.get('title'))
            if not keep_title:
                fresh.meta.pop('title', None)
            self._describe(fresh, profile)
            fresh.save()
        self.checkpoints_changed.emit()

    def adopt_replay(self, cp, profile, folder: Path, wall_seconds: float) -> str:
        """A headless run materialised the snapshot for `profile`: its savestate, report, save-data and warm-up state
        move into the snapshot (called on the worker thread)."""
        report = json.loads((folder / 'report.json').read_text(encoding='utf-8'))
        with self._store_lock:
            cp = self.store.get(cp.name) or cp   # the newest metadata (the other build may have finished first)
            cp.state_file(profile).parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(folder / 'state.gekkostate'), cp.state_file(profile))
            shutil.copy2(folder / 'report.json', cp.report_file(profile))
            snapshot_save_data(folder, cp.save_data_dir(profile))
            if cp.meta.get('saved_tick') is None:
                cp.meta['saved_tick'] = report['game_ticks']
            if cp.recipe.get('stop_tick') is None and report.get('boundary_tick'):
                cp.recipe['stop_tick'] = report['boundary_tick']
            if cp.recipe.get('frame_tick') is None:
                cp.recipe['frame_tick'] = int(report['game_ticks']) + 1
            self._record_state(cp, profile, 'replay', Path(self.config['profiles'][profile]['exe']),
                               report['game_ticks'], report.get('boundary_tick'))
            self._take_warmup(cp, profile, folder)
            cp.save()
        verdict = cp.meta.get('verification', {}).get('summary', '')
        return f"{report['game_ticks']} ticks in {wall_seconds:.1f} s; {verdict}"

    def _take_warmup(self, cp, profile, folder: Path):
        warm = folder / 'warmup.gekkostate'
        if not warm.exists():
            return
        shutil.move(str(warm), cp.warmup_file(profile))
        header = read_state_header(cp.warmup_file(profile))
        cp.states.setdefault(profile, {})['warmup'] = {
            'file': f'states/{profile}.warmup.gekkostate', 'game_ticks': header[2] if header else None,
            'asked_tick': cp.recipe.get('warmup_tick'), 'created': now_iso()}

    def adopt_warmup(self, cp, profile, folder: Path, wall_seconds: float) -> str:
        """A same-build replay made the warm-up state; its end state's report is checked against the live save's
        (the replay must reproduce what was played exactly)."""
        report = json.loads((folder / 'report.json').read_text(encoding='utf-8'))
        with self._store_lock:
            cp = self.store.get(cp.name) or cp
            self._take_warmup(cp, profile, folder)
            live = cp.report(profile)
            check = None
            if live:
                rows = probes.compare_reports(live, report)
                mismatched = [r[0] for r in rows if r[3] is False and r[0] != 'memory.blocks_differing']
                differing, total, where = probes.memory_difference(live.get('memory', {}),
                                                                    report.get('memory', {}))
                check = {'ok': not mismatched and differing == 0, 'mismatched': mismatched,
                         'memory_blocks_differing': differing, 'memory_blocks': total, 'where': where[:8],
                         'replay_ticks': report.get('game_ticks')}
                shutil.copy2(folder / 'report.json', cp.path / 'states' / f'{profile}.replay.report.json')
            if profile in cp.states:
                cp.states[profile]['replay_check'] = check
            cp.save()
        if check is None:
            return f'warm-up state in {wall_seconds:.1f} s'
        verdict = ('the replay reproduces the live save exactly' if check['ok'] else
                   f"the replay DIFFERS from the live save: {check['mismatched']} "
                   f"{check['memory_blocks_differing']} memory blocks {check['where']}")
        return f'warm-up state in {wall_seconds:.1f} s; {verdict}'

    def verify(self, cp):
        """Every build's state of the snapshot against the first one's: the boundary, the clocks and the semantic
        probes must agree; the memory digest is recorded (the two renderers write different words)."""
        profiles = [p for p in cp.states if cp.report(p) is not None]
        if not profiles:
            return
        reference = profiles[0]
        ref = cp.report(reference)
        result = {'reference': reference, 'profiles': {}}
        all_ok = True
        for p in profiles:
            rep = cp.report(p)
            rows = probes.compare_reports(ref, rep)
            mismatched = [r[0] for r in rows if r[3] is False and r[0] != 'memory.blocks_differing']
            mem = next((r for r in rows if r[0] == 'memory.blocks_differing'), None)
            ok = not mismatched and rep.get('game_ticks') == cp.meta.get('saved_tick')
            all_ok = all_ok and ok
            result['profiles'][p] = {'ok': ok, 'mismatched': mismatched, 'memory': mem[1] if mem else None}
            cp.states[p]['verified'] = ok
        if len(profiles) > 1:
            result['summary'] = ('the builds agree (boundary, clocks, probes)' if all_ok else
                                 'MISMATCH: ' + '; '.join(f"{p}: {', '.join(v['mismatched']) or 'ticks'}"
                                                          for p, v in result['profiles'].items() if not v['ok']))
        else:
            result['summary'] = 'one build so far'
        cp.meta['verification'] = result

    def replayable(self, cp) -> bool:
        """Can another build reach this snapshot (a replay needs the input that led there)?"""
        return not (cp.meta.get('kind') == 'one-button' and not cp.recipe.get('tape'))

    def materialise(self, cp, profile, then=None):
        if not self.replayable(cp):
            self.message.emit(f"'{cp.name}' has no input tape (its game's start was replaced, or it recorded none): "
                              f"it exists for {(cp.meta.get('made_from') or {}).get('profile', '?')} only")
            self.materialised.emit(cp.name, profile, False, 'no input tape')
            return
        root = cp.recipe.get('root', {})
        if root.get('kind') == 'checkpoint':
            parent = self.store.get(root.get('name'))
            if parent is None:
                self.materialised.emit(cp.name, profile, False, f"the root snapshot '{root.get('name')}' is gone")
                return
            if not parent.has_state(profile):
                self.message.emit(f"'{cp.name}' needs '{parent.name}' for {profile} first")
                self.materialise(parent, profile,
                                 then=lambda ok: self.materialise(cp, profile, then) if ok else None)
                return
        job = HeadlessRun(self, cp, profile)
        job.done.connect(lambda ok, text, job=job: self._job_done(job, ok, text, then))
        self._jobs.append(job)
        self.message.emit(f"making '{cp.name}' for {profile} (headless replay)...")
        self.sessions_changed.emit()
        job.start()

    def _job_done(self, job, ok, text, then):
        if job in self._jobs:
            self._jobs.remove(job)
        name = job.cp.name
        what = 'warm-up' if getattr(job, 'purpose', '') == 'warmup' else 'state'
        self.message.emit(f"'{name}' {what} for {job.profile_id}: {'done' if ok else 'FAILED'} - {text}")
        self.materialised.emit(name, job.profile_id, ok, text)
        cp = self.store.get(name)
        if ok and cp is not None:
            self.make_thumbnail(cp)
        name = self._apply_pending_rename(name)
        self.checkpoints_changed.emit()
        self.sessions_changed.emit()
        if then:
            then(ok)
        if ok:
            self._recreate_when_ready(name)

    def materialise_all(self, cp):
        for profile in dict.fromkeys(self.profile_of(s) for s in SIDES):
            if not cp.has_state(profile):
                self.materialise(cp, profile)

    def make_thumbnail(self, cp, force=False):
        """The first frame from the snapshot, rendered headless by the AFTER build (else any build with a state)."""
        if cp is None or ((cp.path / 'thumb.png').exists() and not force):
            return
        if any(j.cp.name == cp.name for j in self._thumbs):
            return
        order = [self.profile_of('after'), self.profile_of('before')] + list(cp.states)
        profile = next((p for p in order if cp.has_state(p)), None)
        if profile is None:
            return
        job = ThumbnailJob(self, cp, profile)

        def done(name, ok, text, job=job):
            if job in self._thumbs:
                self._thumbs.remove(job)
            if not ok:
                self.message.emit(f"thumbnail of '{name}': {text}")
            self._apply_pending_rename(name)
            self.checkpoints_changed.emit()

        job.done.connect(done)
        self._thumbs.append(job)
        job.start()

    # ---- recreating the frame (RECREATE EXACT FRAME, RENDER REFERENCE / NATIVE) ----
    def ready_for_recreation(self, cp, profiles=None) -> bool:
        profiles = profiles or [self.profile_of(s) for s in SIDES]
        for p in profiles:
            if not cp.has_state(p):
                return False
            if cp.recipe.get('warmup_tick') and not cp.has_warmup(p):
                return False
        return True

    def _recreate_when_ready(self, name):
        """A recreation asked for while the snapshot was still being made, else the automatic one."""
        wanted = self._recreate_after.pop(name, None)
        cp = self.store.get(name)
        if wanted is not None and cp is not None:
            if any(j.cp.name == name for j in self._jobs):
                self._recreate_after[name] = wanted
                return
            self.recreate(cp, wanted[0], wanted[1])
            return
        self._maybe_auto_recreate(name)

    def _maybe_auto_recreate(self, name):
        if not (self.config.get('recreate') or {}).get('auto', True):
            return
        cp = self.store.get(name)
        if cp is None or cp.meta.get('kind') != 'one-button' or cp.meta.get('recreations'):
            return
        if name in self._recreate_sets or any(j.cp.name == name for j in self._jobs):
            return
        if self.ready_for_recreation(cp):
            self.recreate(cp, SIDES, label='automatic, after the snapshot', auto=True)

    def frame_tick(self, cp) -> int:
        return int(cp.recipe.get('frame_tick') or (cp.saved_tick or 0) + 1)

    def recreate(self, cp, sides=SIDES, label: str = 'RECREATE EXACT FRAME', auto: bool = False):
        """The snapshot's frame rendered again by each side's build from its own warm-up state (or the snapshot's
        state), headless; then compared (results: `recreated`, and the captures on the compare view)."""
        if cp.name in self._recreate_sets:
            self.message.emit(f"'{cp.name}': a recreation is already running")
            return
        if any(j.cp.name == cp.name for j in self._jobs):
            # Its states or warm-up states are still being made: recreated as soon as they are.
            self._recreate_after[cp.name] = (list(sides), label)
            self.message.emit(f"[Recreate] '{cp.name}' is still being made; its frame is recreated when it is ready")
            return
        profiles = list(dict.fromkeys(self.profile_of(s) for s in sides))
        missing = [p for p in profiles if not cp.has_state(p)]
        if missing and not self.replayable(cp):
            profiles = [p for p in profiles if p not in missing]
            self.message.emit(f"[Recreate] '{cp.name}' exists for {', '.join(p.upper() for p in cp.states) or '-'} "
                              f"only (no input tape): {', '.join(m.upper() for m in missing)} cannot render it")
            if not profiles:
                return
            sides = [s for s in sides if self.profile_of(s) in profiles]
        elif missing:
            self.message.emit(f"'{cp.name}' has no state yet for {', '.join(missing)} (making it first)")
            self._recreate_after[cp.name] = (list(sides), label)
            for p in missing:
                if not self._job_for(cp.name, p):
                    self.materialise(cp, p)
            return
        tick = self.frame_tick(cp)
        # One starting point for every build: the warm-up states only when each build that has the snapshot has
        # its warm-up (renders made separately - RENDER REFERENCE, then RENDER NATIVE - stay comparable).
        use_warmup = bool(cp.recipe.get('warmup_tick')) and all(cp.has_warmup(p) for p in cp.states
                                                               if cp.has_state(p))
        self._recreate_sets[cp.name] = {'want': set(profiles), 'results': {}, 'label': label, 'auto': auto,
                                        'tick': tick, 'sides': list(sides)}
        self.message.emit(f"[Recreate] '{cp.name}': rendering the frame at game tick {tick} in "
                          f"{', '.join(p.upper() for p in profiles)} "
                          f"({'from the warm-up states' if use_warmup else 'from the snapshot state'})...")
        for profile in profiles:
            job = RecreateJob(self, cp, profile, tick, label, use_warmup)
            job.done.connect(lambda result, job=job: self._recreate_done(job, result))
            self._recreations.append(job)
            job.start()
        self.sessions_changed.emit()

    def _recreate_done(self, job, result):
        if job in self._recreations:
            self._recreations.remove(job)
        name = job.cp.name
        group = self._recreate_sets.get(name)
        if group is None:
            return
        group['results'][job.profile_id] = result
        if result.get('ok'):
            self.message.emit(f"[Recreate] '{name}' {job.profile_id.upper()}: frame {result.get('frame')} (game tick "
                              f"{result.get('frame_game_ticks')}) from the {result.get('from')} state in "
                              f"{result.get('wall_seconds')} s")
        else:
            self.message.emit(f"[Recreate] '{name}' {job.profile_id.upper()} FAILED: {result.get('error')}")
        if set(group['results']) != group['want']:
            return
        del self._recreate_sets[name]
        self._finish_recreation(name, group)
        self._apply_pending_rename(name)
        self.sessions_changed.emit()

    def _finish_recreation(self, name, group):
        threshold = int(self.config['capture'].get('diff_threshold', 8))
        fit = bool(self.config['capture'].get('fit_before_to_after', True))
        results = group['results']
        with self._store_lock:
            cp = self.store.get(name)
            if cp is None:
                return
            record = {'when': now_iso(), 'label': group['label'], 'frame_tick': group['tick'],
                      'renders': {p: {k: v for k, v in r.items() if k != 'path'} for p, r in results.items()}}
            before, after = self.profile_of('before'), self.profile_of('after')
            rb, ra = results.get(before) or {}, results.get(after) or {}
            if rb.get('ok') and ra.get('ok'):
                stats = compare_files(cp.path / rb['file'], cp.path / ra['file'], threshold, fit)
                same = rb.get('frame_game_ticks') == ra.get('frame_game_ticks')
                record['comparison'] = {'stats': stats, 'same_frame': same}
                cp.meta['last_comparison'] = {
                    'when': record['when'], 'label': f"recreated frame (tick {group['tick']})", 'kind': 'recreate',
                    'same_frame': same, 'frame_ticks': {'before': rb.get('frame_game_ticks'),
                                                        'after': ra.get('frame_game_ticks')},
                    'before': str(cp.path / rb['file']), 'after': str(cp.path / ra['file']), 'stats': stats}
                self.message.emit(f"[Recreate] '{name}' OLD vs NEW: {imagediff.describe(stats)}")
            # The build that was played: its recreation against what was on screen.
            ref = cp.meta.get('reference') or {}
            played = results.get(ref.get('profile')) if ref else None
            if played and played.get('ok') and cp.reference_image() is not None:
                stats = compare_files(cp.path / played['file'], cp.reference_image(), threshold, False)
                same = played.get('frame_game_ticks') == ref.get('frame_game_ticks')
                record['reproduction'] = {'profile': ref['profile'], 'stats': stats, 'same_frame': same,
                                          'identical': bool(stats.get('identical')) and same}
                cp.meta['reproduction'] = record['reproduction']
                verdict = ('EXACT - the recreated frame is the one that was on screen' if
                           record['reproduction']['identical'] else
                           f"differs from the frame on screen: {imagediff.describe(stats)}")
                self.message.emit(f"[Recreate] '{name}' {ref['profile'].upper()} recreated vs live: {verdict}")
            cp.meta.setdefault('recreations', []).append(record)
            cp.meta['recreations'] = cp.meta['recreations'][-12:]
            cp.save()
        payload = {'checkpoint': name, 'label': f"{name}: recreated frame, game tick {group['tick']}",
                   'renders': results, 'record': record}
        sides = group['sides']
        for side in SIDES:
            r = results.get(self.profile_of(side))
            payload[side] = str(cp.path / r['file']) if r and r.get('ok') else None
        if 'comparison' in record:
            payload['same_frame'] = record['comparison']['same_frame']
            payload['frame_ticks'] = cp.meta['last_comparison']['frame_ticks']
            payload['stats'] = record['comparison']['stats']
        if len(sides) == 1:
            # One build rendered: show it against the frame that was on screen (or the other build's last render).
            side = sides[0]
            other = other_side(side)
            payload[other] = self._latest_other(cp, self.profile_of(other))
        self.recreated.emit(payload)
        self.captures_ready.emit(payload)
        self.checkpoints_changed.emit()

    def _latest_other(self, cp, profile):
        """For a one-build render: the frame to compare it with - the other build's latest recreation, else the live
        reference picture."""
        for record in reversed(cp.meta.get('recreations', [])):
            r = record.get('renders', {}).get(profile)
            if r and r.get('ok') and (cp.path / r['file']).exists():
                return str(cp.path / r['file'])
        ref = cp.reference_image()
        return str(ref) if ref is not None else None

    def latest_pair(self, cp):
        """The newest recreated frame of each side (for PIXEL COMPARE), falling back to the live reference."""
        out = {}
        for side in SIDES:
            profile = self.profile_of(side)
            out[side] = None
            for record in reversed(cp.meta.get('recreations', [])):
                r = record.get('renders', {}).get(profile)
                if r and r.get('ok') and (cp.path / r['file']).exists():
                    out[side] = str(cp.path / r['file'])
                    break
        ref = cp.reference_image()
        for side in SIDES:
            if out[side] is None and ref is not None and (cp.meta.get('reference') or {}).get('profile') == \
                    self.profile_of(side):
                out[side] = str(ref)
        return out

    def busy_jobs(self) -> int:
        return len(self._jobs) + len(self._recreations)

    def shutdown(self):
        for side in SIDES:
            s = self.sessions.get(side)
            if s is not None and s.running():
                s.stop()
