"""Running the builds: windowed sessions the workstation drives through the development link, and headless runs that
materialise a checkpoint for a build (docs/comparator/README.md "Sessions").

Every run gets its own session folder with a copy of the executable: the game resolves save-data/, display.cfg and
pad.cfg beside itself, so a run never touches the user's own save files and two runs never share one.
"""
from __future__ import annotations

import ctypes
import datetime
import hashlib
import os
import shutil
import subprocess
import threading
import time
import uuid
from ctypes import wintypes
from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Signal

from . import probes

TICKS_PER_SECOND = 40_500_000
# One display field of game time at the game's 60 Hz (59.94: 40.5 MHz x 1001 / 60000).
FIELD_TICKS = TICKS_PER_SECOND * 1001 // 60000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
ANSI = 'mbcs' if os.name == 'nt' else 'latin-1'


def decode_line(raw: bytes) -> str:
    # The runtime's output is mixed: paths in the ANSI code page, the audio device name in UTF-8.
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError:
        return raw.decode(ANSI, errors='replace')


_sha_cache = {}


def file_sha256(path: Path) -> str:
    path = Path(path)
    try:
        stat = path.stat()
    except OSError:
        return ''
    key = (str(path), stat.st_size, stat.st_mtime_ns)
    if key in _sha_cache:
        return _sha_cache[key]
    h = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    _sha_cache[key] = h.hexdigest()
    return _sha_cache[key]


def read_state_header(path: Path):
    """(version, renderer kind, game ticks) of a savestate, or None."""
    try:
        with Path(path).open('rb') as f:
            head = f.read(20)
    except OSError:
        return None
    if len(head) < 16 or head[:4] != b'SKGK':
        return None
    version = int.from_bytes(head[4:8], 'little')
    ticks = int.from_bytes(head[8:16], 'little')
    kind = int.from_bytes(head[16:20], 'little') if version >= 19 and len(head) >= 20 else 0
    return version, kind, ticks


def stamp() -> str:
    return datetime.datetime.now().strftime('%Y%m%d-%H%M%S')


def prepare_folder(sessions_dir: Path, label: str, exe: Path, save_data: Path, pad_cfg) -> Path:
    folder = Path(sessions_dir) / f'{stamp()}-{label}-{uuid.uuid4().hex[:6]}'
    folder.mkdir(parents=True)
    shutil.copy2(exe, folder / 'ShadowTheHedgehog.exe')
    target = folder / 'save-data'
    if save_data is not None and Path(save_data).is_dir():
        shutil.copytree(save_data, target)
    else:
        target.mkdir()
    if pad_cfg and Path(pad_cfg).is_file():
        shutil.copy2(pad_cfg, folder / 'pad.cfg')
    return folder


def snapshot_save_data(folder: Path, target: Path):
    """Copies a run's SRAM and memory card images (what the checkpoint's launches start from)."""
    source = Path(folder) / 'save-data'
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    if source.is_dir():
        for item in source.iterdir():
            if item.is_file() and (item.name.endswith('.pc.raw') or item.name.endswith('.pc.raw.source')):
                shutil.copy2(item, target / item.name)


def cleanup_sessions(sessions_dir: Path, days: float):
    if days <= 0:
        return
    limit = time.time() - days * 86400
    for path in Path(sessions_dir).iterdir():
        try:
            if path.is_dir() and path.stat().st_mtime < limit:
                shutil.rmtree(path, ignore_errors=True)
        except OSError:
            pass


# ---- windows (Win32) -------------------------------------------------------------------------------------------------

if os.name == 'nt':
    _user32 = ctypes.WinDLL('user32', use_last_error=True)
    _EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    _user32.EnumWindows.argtypes = [_EnumProc, wintypes.LPARAM]
    _user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    _user32.IsWindowVisible.argtypes = [wintypes.HWND]
    _user32.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
    _user32.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                     ctypes.c_int, wintypes.UINT]
    _user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    _user32.SystemParametersInfoW.argtypes = [wintypes.UINT, wintypes.UINT, ctypes.c_void_p, wintypes.UINT]
    _user32.SetForegroundWindow.argtypes = [wintypes.HWND]


def find_window(pid: int):
    if os.name != 'nt':
        return None
    found = []

    def callback(hwnd, _):
        owner = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd):
            found.append(hwnd)
            return False
        return True

    _user32.EnumWindows(_EnumProc(callback), 0)
    return found[0] if found else None


def work_area():
    rect = wintypes.RECT()
    if os.name == 'nt' and _user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rect), 0):  # SPI_GETWORKAREA
        return rect.left, rect.top, rect.right, rect.bottom
    return 0, 0, 1920, 1080


def place_window(hwnd, side: str, title: str):
    """Left half for BEFORE, right half for AFTER (the window keeps its size)."""
    if os.name != 'nt' or not hwnd:
        return
    _user32.SetWindowTextW(hwnd, title)
    rect = wintypes.RECT()
    _user32.GetWindowRect(hwnd, ctypes.byref(rect))
    width = rect.right - rect.left
    left, top, right, _bottom = work_area()
    x = left if side == 'before' else max(left, right - width)
    if side != 'before' and x < left + width:
        x = left + (right - left) // 2
    _user32.SetWindowPos(hwnd, 0, x, top, 0, 0, 0x0001 | 0x0004 | 0x0010)  # NOSIZE | NOZORDER | NOACTIVATE


def focus_window(hwnd):
    if os.name == 'nt' and hwnd:
        _user32.SetForegroundWindow(hwnd)


# ---- a process with its output ----------------------------------------------------------------------------------------

class GameProcess(QObject):
    """One launched executable: its output (to the UI and to files) and its exit."""
    line = Signal(str, str)     # stream ('out' / 'err'), text
    exited = Signal(int)

    def __init__(self, argv, folder: Path, env_extra=None, parent=None):
        super().__init__(parent)
        self.argv = [str(a) for a in argv]
        self.folder = Path(folder)
        self.env = dict(os.environ)
        self.env.update({k: str(v) for k, v in (env_extra or {}).items()})
        self.proc = None
        self.returncode = None

    def start(self):
        (self.folder / 'invocation.txt').write_text(subprocess.list2cmdline(self.argv) + '\n', encoding='utf-8')
        self.proc = subprocess.Popen(self.argv, cwd=self.folder, env=self.env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=NO_WINDOW)
        for stream, name in ((self.proc.stdout, 'out'), (self.proc.stderr, 'err')):
            threading.Thread(target=self._pump, args=(stream, name), daemon=True).start()
        threading.Thread(target=self._wait, daemon=True).start()

    @property
    def pid(self):
        return self.proc.pid if self.proc else None

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def _pump(self, stream, name):
        path = self.folder / ('stdout.txt' if name == 'out' else 'stderr.txt')
        with path.open('ab') as log:
            for raw in iter(stream.readline, b''):
                log.write(raw)
                log.flush()
                self.line.emit(name, decode_line(raw).rstrip('\r\n'))

    def _wait(self):
        code = self.proc.wait()
        self.returncode = code
        self.exited.emit(code)

    def terminate(self):
        if self.running():
            self.proc.terminate()


# ---- a windowed session -----------------------------------------------------------------------------------------------

class GameSession(QObject):
    """A windowed run of one build profile on the BEFORE or AFTER side, driven through the development link."""
    status_changed = Signal()
    log = Signal(str)
    event = Signal(dict)
    finished = Signal(int)

    def __init__(self, app, side: str, profile_id: str, start: dict, parent=None):
        """`start`: {'kind': 'checkpoint', 'checkpoint': Checkpoint} or a route {'kind': 'boot-stage', 'stage': ..}."""
        super().__init__(parent)
        self.app = app
        self.side = side
        self.profile_id = profile_id
        self.profile = app.config['profiles'][profile_id]
        self.start_point = start
        self.token = f'{side}-{uuid.uuid4().hex[:10]}'
        self.folder = None
        self.process = None
        self.connection = None
        self.hello = {}
        self.status = 'idle'
        self.paused = False
        self.last_status = {}
        self.hwnd = None
        self.root_valid = True        # False once the starting point is replaced outside the workstation (M)
        self.root_state = None        # the state file this session started from (for Reload)
        self._place_timer = None
        self.trace = True             # every control request and its answer in the side's log

    # ---- launch ----
    def launch(self):
        cfg = self.app.config
        exe = Path(self.profile['exe'])
        if not exe.is_file():
            raise FileNotFoundError(f'{exe} does not exist (build it, or fix the profile in Settings)')
        start = self.start_point
        if start['kind'] == 'checkpoint':
            cp = start['checkpoint']
            if not cp.has_state(self.profile_id):
                raise RuntimeError(f"checkpoint '{cp.name}' has no state for '{self.profile_id}' yet: materialise it")
            save_data = cp.save_data_dir(self.profile_id)
        else:
            save_data = self.app.store.ensure_seed(self.app.seed_candidates())
        self.folder = prepare_folder(self.app.store.sessions_dir, f'{self.side}-{self.profile_id}', exe, save_data,
                                     cfg.get('pad_cfg'))
        win = cfg['window']
        argv = [self.folder / 'ShadowTheHedgehog.exe', '--game-data', cfg['game_data'], '--windowed',
                '--width', win['width'], '--height', win['height']]
        audio = win.get('audio', 'after')
        argv += ['--audio'] if audio in (self.side, 'both') else ['--no-audio']
        live_input = win.get('input', 'both')
        if live_input not in (self.side, 'both'):
            argv += ['--no-xinput']
        if self.profile.get('dev_link', True):
            argv += ['--dev-link', self.app.devlink.endpoint, '--dev-link-name', self.token, '--start-paused',
                     '--input-tape-record', self.folder / 'session.tape']
            # One-button snapshots: the key, where the game writes them (the workstation adopts them into the
            # store), and the probes their state reports carry.
            argv += ['--snapshot-key', cfg.get('snapshot_hotkey') or 'F5', '--snapshot-dir', self.folder / 'snapshots',
                     '--report-peek', probes.peek_list()]
        if start['kind'] == 'checkpoint':
            quick = self.folder / 'quick.gekkostate'
            shutil.copy2(start['checkpoint'].state_file(self.profile_id), quick)
            self.root_state = quick
            argv += ['--state-file', quick, '--load-state']
        else:
            argv += ['--state-file', self.folder / 'quick.gekkostate']
            argv += route_args(start)
        argv += list(self.profile.get('args', [])) + list(self.profile.get('window_args', []))
        self.process = GameProcess(argv, self.folder, self.profile.get('env'))
        self.process.line.connect(self._line)
        self.process.exited.connect(self._exited)
        self.app.register_session(self)
        self.process.start()
        self._set_status('starting')
        self._place_timer = QTimer(self)
        self._place_timer.setInterval(250)
        self._place_attempts = 0
        self._place_timer.timeout.connect(self._try_place)
        self._place_timer.start()
        self.log.emit(f'launched {exe.name} ({self.profile_id}) in {self.folder}')

    def _try_place(self):
        self._place_attempts += 1
        if self.process is None or not self.process.running() or self._place_attempts > 80:
            self._place_timer.stop()
            return
        hwnd = find_window(self.process.pid)
        if hwnd:
            self.hwnd = hwnd
            label = 'BEFORE' if self.side == 'before' else 'AFTER'
            title = f"{label} - {self.profile.get('label', self.profile_id)} - {self.start_label()}"
            if self.app.config['window'].get('side_by_side', True):
                place_window(hwnd, self.side, title)
            self._place_timer.stop()

    def start_label(self) -> str:
        start = self.start_point
        return start['checkpoint'].name if start['kind'] == 'checkpoint' else describe_route(start)

    # ---- the link ----
    def attach(self, connection):
        self.connection = connection
        self.hello = connection.info or {}
        self.paused = bool(self.hello.get('paused'))
        self.last_status = dict(self.hello)
        connection.event.connect(self._event)
        connection.closed.connect(self._link_closed)
        self._set_status('paused' if self.paused else 'running')
        self.log.emit(f"connected: {self.hello.get('flavour')} build, savestate v{self.hello.get('savestate_version')}"
                      f", {self.hello.get('renderer')}")

    def request(self, cmd, callback=None, **fields):
        if self.connection is None:
            if callback:
                callback({'ok': False, 'error': 'not connected'})
            return

        def done(reply):
            if 'paused' in reply:
                self.paused = bool(reply['paused'])
                self.last_status = reply
                self._set_status('paused' if self.paused else 'running')
            if not reply.get('ok'):
                self.log.emit(f"{cmd}: {reply.get('error', 'failed')}")
            if self.trace and cmd not in ('status', 'report'):
                self.log.emit(f"[link] {cmd} -> ok={reply.get('ok')} paused={reply.get('paused')} "
                              f"tick={reply.get('game_ticks')} capture_tick={reply.get('capture_game_ticks')}")
            if callback:
                callback(reply)

        if self.trace and cmd not in ('status', 'report'):
            shown = {k: v for k, v in fields.items() if k not in ('peeks',)}
            self.log.emit(f'[link] {cmd} {shown}')
        self.connection.request(cmd, done, **fields)

    def _event(self, message):
        if message.get('event') == 'loaded' and message.get('source') == 'hotkey':
            self.root_valid = False
            self.log.emit('the quick slot was loaded with M: checkpoints made in this session now exist only for '
                          'this build')
        self.event.emit(message)

    def _link_closed(self):
        self.connection = None
        if self.process is not None and self.process.running():
            self._set_status('unlinked')

    def _line(self, stream, text):
        self.log.emit(text if stream == 'out' else '[stderr] ' + text)

    def _exited(self, code):
        self._set_status(f'exited ({code})')
        self.connection = None
        self.finished.emit(code)

    def _set_status(self, status):
        self.status = status
        self.status_changed.emit()

    def running(self):
        return self.process is not None and self.process.running()

    def stop(self):
        if self.connection is not None:
            self.request('quit')
            QTimer.singleShot(4000, self._kill_if_running)
        else:
            self._kill_if_running()

    def _kill_if_running(self):
        if self.process is not None and self.process.running():
            self.process.terminate()


def route_args(start: dict):
    kind = start.get('kind')
    args = []
    if kind == 'boot-stage':
        args += ['--boot-stage', start['stage']]
    elif kind == 'boot-story':
        args += ['--boot-story']
    if start.get('ui_test_script'):
        args += ['--ui-test-script', start['ui_test_script']]
    return args


def describe_route(start: dict) -> str:
    kind = start.get('kind')
    if kind == 'boot-stage':
        return f"boot {start['stage']}"
    if kind == 'boot-story':
        return 'boot story'
    return 'cold boot'


# ---- headless materialisation -----------------------------------------------------------------------------------------

class HeadlessRun(QObject):
    """Materialises a checkpoint for one build: from the recipe's root (that build's own state of the parent, or a
    cold boot / route), the recipe's input, to the boundary after `stop_tick`; the build saves its own savestate and
    state report - and, when the recipe names a `warmup_tick`, the warm-up state at the boundary after it on the way
    (the run goes on). Purpose 'warmup': the build whose state is the live save replays only for its warm-up state
    (and a report of the replayed end state: the same-build replay must reach the live save's state exactly).
    Runs on a worker thread; signals come back to the UI."""
    log = Signal(str)
    done = Signal(bool, str)

    def __init__(self, app, checkpoint, profile_id, purpose: str = 'materialise', parent=None):
        super().__init__(parent)
        self.app = app
        self.cp = checkpoint
        self.profile_id = profile_id
        self.profile = app.config['profiles'][profile_id]
        self.purpose = purpose
        self.folder = None

    def build_argv(self):
        cfg = self.app.config
        recipe = self.cp.recipe
        exe = Path(self.profile['exe'])
        if not exe.is_file():
            raise FileNotFoundError(f'{exe} does not exist')
        root = recipe.get('root', {})
        if root.get('kind') == 'checkpoint':
            parent = self.app.store.get(root['name'])
            if parent is None or not parent.has_state(self.profile_id):
                raise RuntimeError(f"the root checkpoint '{root.get('name')}' has no state for '{self.profile_id}'")
            save_data = parent.save_data_dir(self.profile_id)
        else:
            parent = None
            save_data = self.app.store.ensure_seed(self.app.seed_candidates())
        self.folder = prepare_folder(self.app.store.sessions_dir, f'replay-{self.profile_id}', exe, save_data, None)
        width, height = recipe.get('display') or (cfg['window']['width'], cfg['window']['height'])
        argv = [self.folder / 'ShadowTheHedgehog.exe', '--game-data', cfg['game_data'], '--headless', '--no-xinput',
                '--ax-mixer', '--job-workers', cfg['headless'].get('job_workers', 4),
                '--width', width, '--height', height]
        if parent is not None:
            argv += ['--state-file', parent.state_file(self.profile_id), '--load-state']
        else:
            argv += route_args(root)
        if recipe.get('ui_test_script'):
            argv += ['--ui-test-script', self.cp.path / recipe['ui_test_script']]
        if recipe.get('tape'):
            argv += ['--input-tape-play', self.cp.path / recipe['tape']]
        if recipe.get('stop_tick'):
            argv += ['--stop-at-tick', recipe['stop_tick']]
        warmup = recipe.get('warmup_tick')
        if warmup and (not recipe.get('stop_tick') or int(warmup) < int(recipe['stop_tick'])):
            argv += ['--save-state-at-tick', f"{int(warmup)}:{self.folder / 'warmup.gekkostate'}"]
        if self.purpose != 'warmup':
            argv += ['--save-state', '--save-state-file', self.folder / 'state.gekkostate']
        argv += ['--state-report', self.folder / 'report.json', '--report-peek', probes.peek_list()]
        argv += list(self.profile.get('args', [])) + list(self.profile.get('headless_args', []))
        return argv

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        try:
            argv = self.build_argv()
        except Exception as error:  # noqa: BLE001 - reported to the UI
            self.done.emit(False, str(error))
            return
        self.log.emit(f'materialising {self.cp.name} for {self.profile_id}: ' + subprocess.list2cmdline(map(str, argv)))
        process = GameProcess(argv, self.folder, self.profile.get('env'))
        process.line.connect(lambda stream, text: self.log.emit(text if stream == 'out' else '[stderr] ' + text))
        start = time.perf_counter()
        process.start()
        timeout = self.app.config['headless'].get('timeout_s', 1800)
        while process.running() and time.perf_counter() - start < timeout:
            time.sleep(0.2)
        if process.running():
            process.terminate()
            self.done.emit(False, f'timed out after {timeout} s')
            return
        while process.returncode is None:
            time.sleep(0.05)
        if process.returncode != 0:
            self.done.emit(False, f'exit code {process.returncode}; logs in {self.folder}')
            return
        try:
            if self.purpose == 'warmup':
                message = self.app.adopt_warmup(self.cp, self.profile_id, self.folder, time.perf_counter() - start)
            else:
                message = self.app.adopt_replay(self.cp, self.profile_id, self.folder, time.perf_counter() - start)
        except Exception as error:  # noqa: BLE001
            self.done.emit(False, str(error))
            return
        self.done.emit(True, message)
