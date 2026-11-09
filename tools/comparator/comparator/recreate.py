"""Recreating a snapshot's frame in a build (docs/comparator/README.md "Recreating a frame").

RECREATE EXACT FRAME renders the snapshot's moment again in each build, headless and deterministic:

    the build's warm-up state (some fields before the snapshot; else the snapshot's own state)
    -> the snapshot's input tape from there (the game's own clock latches it where the play latched it)
    -> the frame the build finishes at or after the snapshot's frame tick, captured (--capture-at-tick: armed before
       the first instruction, so the lifted renderer also draws every frame that saves a view's history and the
       capture shows the trail, the radial blur's saved screen etc. as they were)
    -> the run ends once it is written (--stop-after-capture)

Both builds reach the same game state from their own savestates (proved by the state reports); the two frames are
then two renderings of the same render inputs. The frame tick is the one the build being played captured live
(snapshot_reference), so the recreated frame of that build can also be compared with what was on screen.
"""
from __future__ import annotations

import json
import shutil
import threading
import time
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from . import imagediff, probes
from .sessions import FIELD_TICKS, GameProcess, prepare_folder


class RecreateJob(QObject):
    """One build's rendering of a snapshot's frame. Runs on a worker thread; `done` comes back to the UI."""
    done = Signal(object)    # the result dict (ok, profile, file, frame_game_ticks, ...)

    def __init__(self, app, checkpoint, profile_id, frame_tick: int, label: str, use_warmup: bool = True,
                 parent=None):
        super().__init__(parent)
        self.app = app
        self.cp = checkpoint
        self.profile_id = profile_id
        self.profile = app.config['profiles'][profile_id]
        self.frame_tick = int(frame_tick)
        self.label = label
        self.use_warmup = use_warmup
        self.folder = None

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _start_state(self):
        if self.use_warmup and self.cp.has_warmup(self.profile_id):
            st = self.cp.states[self.profile_id]['warmup']
            return self.cp.warmup_file(self.profile_id), 'warm-up', st.get('game_ticks')
        st = self.cp.states.get(self.profile_id) or {}
        return self.cp.state_file(self.profile_id), 'snapshot', st.get('game_ticks')

    def _run(self):
        result = {'ok': False, 'profile': self.profile_id, 'checkpoint': self.cp.name, 'label': self.label,
                  'frame_tick': self.frame_tick}
        try:
            result.update(self._render())
        except Exception as error:  # noqa: BLE001 - reported to the UI
            result['error'] = str(error)
        self.done.emit(result)

    def _render(self):
        cfg = self.app.config
        exe = Path(self.profile['exe'])
        if not exe.is_file():
            raise FileNotFoundError(f'{exe} does not exist')
        state, origin, origin_tick = self._start_state()
        if not state.exists():
            raise RuntimeError(f"'{self.cp.name}' has no {self.profile_id} state to start from")
        self.folder = prepare_folder(self.app.store.sessions_dir, f'recreate-{self.profile_id}', exe,
                                     self.cp.save_data_dir(self.profile_id), None)
        width, height = self.cp.recipe.get('display') or (cfg['window']['width'], cfg['window']['height'])
        margin = int((cfg.get('recreate') or {}).get('stop_margin_fields', 40))
        argv = [self.folder / 'ShadowTheHedgehog.exe', '--game-data', cfg['game_data'], '--headless', '--no-xinput',
                '--ax-mixer', '--job-workers', cfg['headless'].get('job_workers', 4), '--width', width,
                '--height', height, '--state-file', state, '--load-state']
        if (self.cp.states.get(self.profile_id) or {}).get('renderer_kind') == 1:
            # The legacy renderer chooses its EFB scale from the output it presents to: the window when there is
            # one, else --native-output (else the console's 640x480, 1x). The snapshot's window size makes the
            # recreation's EFB the one the played session showed (928x522 -> 2x, a 1280x960 EFB).
            argv += ['--native-output', f'{width}x{height}']
        if self.cp.recipe.get('tape'):
            argv += ['--input-tape-play', self.cp.path / self.cp.recipe['tape']]
        if self.cp.recipe.get('ui_test_script'):
            argv += ['--ui-test-script', self.cp.path / self.cp.recipe['ui_test_script']]
        argv += ['--capture-at-tick', f"{self.frame_tick}:{self.folder / 'frame'}", '--stop-after-capture',
                 '--stop-at-tick', self.frame_tick + margin * FIELD_TICKS,
                 '--state-report', self.folder / 'report.json', '--report-peek', probes.peek_list()]
        argv += list(self.profile.get('args', [])) + list(self.profile.get('headless_args', []))
        process = GameProcess(argv, self.folder, self.profile.get('env'))
        start = time.perf_counter()
        process.start()
        timeout = cfg['headless'].get('timeout_s', 1800)
        while process.running() and time.perf_counter() - start < timeout:
            time.sleep(0.1)
        if process.running():
            process.terminate()
            raise RuntimeError(f'the recreation did not finish in {timeout} s ({self.folder})')
        while process.returncode is None:
            time.sleep(0.05)
        record_path = self.folder / 'frame.capture.json'
        if process.returncode != 0 or not record_path.exists():
            raise RuntimeError(f'exit code {process.returncode}; logs in {self.folder}')
        record = json.loads(record_path.read_text(encoding='utf-8'))
        if not record.get('written'):
            raise RuntimeError(f"the frame at tick {self.frame_tick} was not rendered (ended at tick "
                               f"{record.get('ended_at_tick')}); logs in {self.folder}")
        image = imagediff.load(record['path'])
        renders = self.cp.path / 'renders'
        renders.mkdir(exist_ok=True)
        stamp = time.strftime('%Y%m%d-%H%M%S')
        out = renders / f'{stamp}-{self.profile_id}.png'
        imagediff.save_png(image, out)
        report = None
        if (self.folder / 'report.json').exists():
            report = json.loads((self.folder / 'report.json').read_text(encoding='utf-8'))
            shutil.copy2(self.folder / 'report.json', renders / f'{stamp}-{self.profile_id}.report.json')
        wall = time.perf_counter() - start
        shutil.rmtree(self.folder, ignore_errors=True)
        return {'ok': True, 'file': str(out.relative_to(self.cp.path)).replace('\\', '/'), 'path': str(out),
                'frame': record.get('frame'), 'frame_game_ticks': record.get('frame_game_ticks'),
                'size': [int(image.shape[1]), int(image.shape[0])], 'from': origin, 'from_tick': origin_tick,
                'wall_seconds': round(wall, 2), 'report_game_ticks': report.get('game_ticks') if report else None,
                'report': f'renders/{stamp}-{self.profile_id}.report.json' if report else None}


def compare_files(a: Path, b: Path, threshold: int, fit: bool = True) -> dict:
    """The difference statistics of two frames (the first fitted to the second's size when `fit`; else compared as
    they are - a size mismatch is recorded, the common crop compared)."""
    first, second = imagediff.load(a), imagediff.load(b)
    x, y = imagediff.pair(first, second, fit)
    _image, stats = imagediff.difference(x, y, threshold)
    if first.shape != second.shape:
        stats['sizes'] = [[int(first.shape[1]), int(first.shape[0])], [int(second.shape[1]), int(second.shape[0])]]
        if not fit:
            stats['identical'] = False
    return stats
