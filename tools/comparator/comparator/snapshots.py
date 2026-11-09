"""User snapshots on top of the checkpoint store (docs/comparator/README.md "Snapshots").

A user snapshot is a comparison checkpoint made at any moment of play: the build being played saves its own
savestate at the next traversal boundary and the input tape that led there from the session's start; the other build
reaches the same moment by replaying that tape from its own state of the session's start. What is restorable is the
whole game - every object the game owns (player, enemies and their AI, physics, animation, particles, cameras,
lights, fog, timers, scripts, the random generator) lives in the game's own memory and native state, and each build's
savestate carries all of it in that build's terms. The thumbnail and the semantic summary are metadata.

Thumbnails are the first frame a build renders from the snapshot, made headless (deterministic, at any time - also for
snapshots made from routes or derived from others).
"""
from __future__ import annotations

import datetime
import shutil
import subprocess
import threading
import time
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from . import imagediff, probes
from .sessions import GameProcess, prepare_folder

THUMB_SIZE = (320, 180)
SUMMARY_PROBES = ('stage.number', 'game.time_remaining', 'player1.rings', 'player1.score', 'camera.eye',
                  'camera.hfov', 'player1.class', 'player1.position')


def temporary_name() -> str:
    return 'snap-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S')


def summary(report: dict, stages=()) -> dict:
    """The snapshot's at-a-glance facts from a state report."""
    if not report:
        return {}
    values = probes.decode_report(report)
    stage_no = values.get('stage.number')
    stage_name = ''
    for stg, name, _bootable in stages:
        if stage_no is not None and stg == f'stg{stage_no:04d}':
            stage_name = name
    return {
        'stage': f'stg{stage_no:04d}' if isinstance(stage_no, int) and stage_no >= 0 else None,
        'stage_name': stage_name,
        'game_ticks': report.get('game_ticks'),
        'game_seconds': report.get('game_seconds'),
        'vi_frames': report.get('vi_frames'),
        'rings': values.get('player1.rings'),
        'score': values.get('player1.score'),
        'camera_eye': values.get('camera.eye'),
        'camera_hfov': values.get('camera.hfov'),
        'time_remaining': values.get('game.time_remaining'),
        'player_position': values.get('player1.position') if values.get('player1.class') == '80522d70' else None,
    }


class ThumbnailJob(QObject):
    """Renders the first frame from a snapshot headless with one build and keeps a small PNG of it."""
    done = Signal(str, bool, str)  # checkpoint name, ok, text

    def __init__(self, app, checkpoint, profile_id, parent=None):
        super().__init__(parent)
        self.app = app
        self.cp = checkpoint
        self.profile_id = profile_id

    def start(self):
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self):
        try:
            self._render()
        except Exception as error:  # noqa: BLE001 - reported to the UI
            self.done.emit(self.cp.name, False, str(error))

    def _render(self):
        cfg = self.app.config
        profile = cfg['profiles'][self.profile_id]
        exe = Path(profile['exe'])
        folder = prepare_folder(self.app.store.sessions_dir, f'thumb-{self.profile_id}', exe,
                                self.cp.save_data_dir(self.profile_id), None)
        width, height = self.cp.recipe.get('display') or (cfg['window']['width'], cfg['window']['height'])
        lifted = (self.cp.states.get(self.profile_id) or {}).get('renderer_kind') == 2
        shot = folder / ('first.png' if lifted else 'first.ppm')
        argv = [folder / 'ShadowTheHedgehog.exe', '--game-data', cfg['game_data'], '--headless', '--no-xinput',
                '--ax-mixer', '--width', width, '--height', height, '--state-file',
                self.cp.state_file(self.profile_id), '--load-state', '--budget-ms', '250',
                '--native-screenshot', shot]
        # The lifted renderer numbers frames from 0 in each process, the legacy one its presents from 1.
        argv += ['--native-screenshot-frame', '0' if lifted else '1']
        if lifted:
            argv += ['--native-output', f'{width}x{height}']
        argv += list(profile.get('args', [])) + list(profile.get('headless_args', []))
        process = GameProcess(argv, folder, profile.get('env'))
        process.start()
        start = time.perf_counter()
        while process.running() and time.perf_counter() - start < 120:
            time.sleep(0.1)
        if process.running():
            process.terminate()
            raise RuntimeError('the thumbnail run did not finish')
        while process.returncode is None:
            time.sleep(0.05)
        if not shot.exists():
            raise RuntimeError(f'no first frame written (exit {process.returncode}; {folder})')
        image = imagediff.load(shot)
        if not lifted and 'width' in locals():
            image = imagediff.fit(image, int(width), int(height))
        full = self.cp.path / 'first-frame.png'
        imagediff.save_png(image, full)
        imagediff.save_png(imagediff.fit(image, *THUMB_SIZE), self.cp.path / 'thumb.png')
        shutil.rmtree(folder, ignore_errors=True)
        self.done.emit(self.cp.name, True, f'thumbnail from {self.profile_id}')


def ensure_hidden_console():
    return getattr(subprocess, 'CREATE_NO_WINDOW', 0)
