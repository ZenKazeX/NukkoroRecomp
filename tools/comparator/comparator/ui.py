"""The main window: PLAY, the snapshot library, the selected snapshot's actions (load, recreate, render, compare), the
two sides' CURRENT GAME panels, the shared deterministic controls, the comparison and the logs."""
from __future__ import annotations

import html
import os
from pathlib import Path

from PySide6.QtCore import QSize, Qt, QTimer
from PySide6.QtGui import QAction, QColor, QFont, QIcon, QPainter, QPixmap
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QGridLayout, QGroupBox, QHBoxLayout,
                               QHeaderView, QInputDialog, QLabel, QLineEdit, QListWidget, QListWidgetItem,
                               QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QSizePolicy, QSpinBox,
                               QSplitter, QTableWidget, QTableWidgetItem, QTabWidget, QTextBrowser, QVBoxLayout,
                               QWidget)

from . import imagediff, probes, semantics
from .app import SIDES, Comparator
from .checkpoints import valid_name
from .compare_view import MODES, CompareView
from .dialogs import NewCheckpointDialog, RenameDialog, SettingsDialog, show_json
from .sessions import FIELD_TICKS, TICKS_PER_SECOND, file_sha256, focus_window

STATUS_COLOURS = {'idle': '#6b6b75', 'starting': '#d9a400', 'running': '#2fb344', 'paused': '#3b82f6',
                  'unlinked': '#d97706'}
BIG = 'font-weight: bold; padding: 6px'


def placeholder_thumb() -> QIcon:
    pixmap = QPixmap(160, 90)
    pixmap.fill(QColor(28, 28, 34))
    painter = QPainter(pixmap)
    painter.setPen(QColor(120, 120, 130))
    painter.drawText(pixmap.rect(), Qt.AlignmentFlag.AlignCenter, 'frame\nbeing made')
    painter.end()
    return QIcon(pixmap)


def seconds(ticks) -> str:
    return '-' if ticks is None else f'{ticks / TICKS_PER_SECOND:.3f} s'


def fmt(value) -> str:
    return probes.format_value(value)


def esc(value) -> str:
    return html.escape(str(value)) if value is not None else '-'


def vec(v, digits=2) -> str:
    return '(' + ', '.join(f'{x:.{digits}f}' for x in v) + ')' if v else '-'


class LogView(QPlainTextEdit):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setMaximumBlockCount(8000)
        self.setFont(QFont('Consolas', 9))

    def add(self, text: str):
        self.appendPlainText(text)


class SidePanel(QGroupBox):
    """One side: its build, its CURRENT GAME, its controls."""

    def __init__(self, window, side: str):
        super().__init__()
        self.window = window
        self.app = window.app
        self.side = side
        self.info = {}
        layout = QVBoxLayout(self)
        self.title = QLabel()
        self.title.setStyleSheet('font-size: 15px; font-weight: bold')
        self.title.setWordWrap(True)
        self.exe = QLabel()
        self.exe.setWordWrap(True)
        self.exe.setStyleSheet('color: #888')
        self.status = QLabel()
        self.status.setWordWrap(True)
        self.game = QLabel()
        self.game.setWordWrap(True)
        self.game.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.game.setStyleSheet('font-family: Consolas; font-size: 11px')
        for label in (self.title, self.exe, self.status, self.game):
            # Long paths and values wrap inside the column instead of widening the window.
            label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        layout.addWidget(self.title)
        layout.addWidget(self.exe)
        layout.addWidget(self.status)
        layout.addWidget(self.game)
        grid = QGridLayout()
        self.load = QPushButton()
        self.load.setStyleSheet('font-weight: bold; padding: 5px')
        self.capture = QPushButton()
        self.snapshot = QPushButton()
        self.snapshot.setStyleSheet('font-weight: bold; padding: 5px')
        self.reload = QPushButton('Reload')
        self.pause = QPushButton('Pause')
        self.resume = QPushButton('Resume')
        self.step_tick = QPushButton('Step tick')
        self.step_frame = QPushButton('Step frame')
        self.launch_route = QPushButton('Play from a route...')
        self.stop = QPushButton('Stop')
        self.focus = QPushButton('Focus window')
        self.make = QPushButton('Make its state')
        buttons = (self.load, self.snapshot, self.reload, self.capture, self.pause, self.resume, self.step_tick,
                   self.step_frame, self.launch_route, self.focus, self.stop, self.make)
        for i, b in enumerate(buttons):
            grid.addWidget(b, i // 2, i % 2)
        layout.addLayout(grid)
        layout.addStretch(1)
        self.load.clicked.connect(lambda: window.load_side(side))
        self.capture.clicked.connect(lambda: self.app.run_lockstep([side], 2 * FIELD_TICKS, True, 'capture'))
        self.snapshot.clicked.connect(lambda: window.create_snapshot(side))
        self.reload.clicked.connect(lambda: self.app.reload(side))
        self.pause.clicked.connect(lambda: self.app.pause(side))
        self.resume.clicked.connect(lambda: self.app.resume(side))
        self.step_tick.clicked.connect(lambda: self.app.step_tick([side]))
        self.step_frame.clicked.connect(lambda: self.app.run_lockstep([side], FIELD_TICKS, False, 'step frame'))
        self.launch_route.clicked.connect(lambda: window.launch_side_route(side))
        self.stop.clicked.connect(lambda: self.app.stop(side))
        self.focus.clicked.connect(self._focus)
        self.make.clicked.connect(lambda: window.materialise_selected(side))

    def _focus(self):
        s = self.app.sessions.get(self.side)
        if s is not None:
            focus_window(s.hwnd)

    def set_info(self, info):
        self.info = info
        self.refresh()

    def refresh(self):
        pid = self.app.profile_of(self.side)
        prof = self.app.config['profiles'].get(pid, {})
        where = 'BEFORE (left)' if self.side == 'before' else 'AFTER (right)'
        self.title.setText(f'{pid.upper()}   <span style="color:#888;font-size:12px">{where}: '
                           f"{prof.get('label', '')}</span>")
        exe = Path(prof.get('exe', ''))
        self.exe.setText(str(exe) + ('' if exe.is_file() else '   (MISSING - build it or fix Settings)'))
        self.load.setText(f'LOAD {pid.upper()}')
        self.capture.setText(f'CAPTURE {pid.upper()}')
        self.snapshot.setText(f'Create Snapshot ({self.app.snapshot_key})')
        s = self.app.sessions.get(self.side)
        status = s.status if s is not None else 'not running'
        colour = STATUS_COLOURS.get(status, '#b91c1c' if status.startswith('exited') else '#6b6b75')
        self.status.setText(f"<span style='color:{colour};font-size:15px'>&#9679;</span> <b>{status}</b>"
                            + (f"  -  from {s.start_label()}" if s is not None else ''))
        connected = s is not None and s.connection is not None
        lines = ['CURRENT GAME']
        st = s.last_status if s is not None else {}
        info = self.info if connected else {}
        if connected:
            stage = info.get('stage') or '-'
            lines.append(f"Stage:  {stage} {info.get('stage_name') or self.app.stage_name(stage)}")
            lines.append(f"Tick:   {st.get('game_ticks')}  ({seconds(st.get('game_ticks'))} of game time)")
            lines.append(f"Clocks: VI field {st.get('vi_frames')}, render frame {st.get('render_frames')}, "
                         f"traversal {st.get('traversals')}")
            lines.append(f"Player: {fmt(info.get('player_position'))}, rings {fmt(info.get('rings'))}, "
                         f"score {fmt(info.get('score'))}")
            lines.append(f"Camera: eye {fmt(info.get('camera_eye'))}, hFOV {fmt(info.get('camera_hfov'))} rad")
            if st.get('tape_playing'):
                lines.append('Input:  a tape (live input ignored until Resume)')
            else:
                lines.append(f"Keys:   {self.app.snapshot_key} = snapshot this moment (in the game window)")
            if s.hello:
                lines.append(f"Build:  {s.hello.get('flavour')}, savestate v{s.hello.get('savestate_version')}, "
                             f"{s.hello.get('renderer')}")
            if not s.root_valid:
                lines.append('NOTE:   its start was replaced by an M quick-load')
        else:
            lines.append('(no game running on this side)')
        self.game.setText('\n'.join(lines))
        cp = self.window.selected()
        has_state = cp is not None and cp.has_state(pid)
        self.load.setEnabled(has_state)
        self.load.setToolTip('' if has_state else 'The selected snapshot has no state for this build yet')
        for b in (self.reload, self.pause, self.resume, self.step_tick, self.step_frame, self.capture, self.snapshot):
            b.setEnabled(connected)
        self.stop.setEnabled(s is not None and s.running())
        self.focus.setEnabled(s is not None and s.hwnd is not None)
        self.make.setEnabled(cp is not None and not has_state)


class MainWindow(QMainWindow):
    def __init__(self, app: Comparator):
        super().__init__()
        self.app = app
        self.setWindowTitle('Shadow Native Rewrite - Before / After Comparator')
        self.resize(1600, 1020)

        # ---- menu ----
        menu = self.menuBar()
        file_menu = menu.addMenu('&File')
        for text, slot in (('Settings...', self.settings), ('Open the snapshot store', self.open_store),
                           ('Open the selected snapshot folder', self.open_selected),
                           ('Open the sessions folder', lambda: self._open(self.app.store.sessions_dir))):
            action = QAction(text, self)
            action.triggered.connect(slot)
            file_menu.addAction(action)
        file_menu.addSeparator()
        quit_action = QAction('Quit', self)
        quit_action.triggered.connect(self.close)
        file_menu.addAction(quit_action)
        help_menu = menu.addMenu('&Help')
        about = QAction('How it works', self)
        about.triggered.connect(self.help)
        help_menu.addAction(about)

        # ---- PLAY ----
        left = QWidget()
        ll = QVBoxLayout(left)
        play = QGroupBox('PLAY  -  then press the snapshot key in the game window at any moment')
        pl = QGridLayout(play)
        self.play_stage = QComboBox()
        self.play_stage.setToolTip('Where the game starts: a stage straight from boot, the story, or a cold boot')
        self.play_old = QPushButton()
        self.play_new = QPushButton()
        self.play_from_snapshot = QPushButton('Play the reference build from the selected snapshot')
        for b in (self.play_old, self.play_new):
            b.setStyleSheet(BIG)
        self.play_hint = QLabel()
        self.play_hint.setWordWrap(True)
        self.play_hint.setStyleSheet('color: #888')
        self.play_hint.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        pl.addWidget(QLabel('Start'), 0, 0)
        pl.addWidget(self.play_stage, 0, 1, 1, 2)
        pl.addWidget(self.play_old, 1, 0, 1, 2)
        pl.addWidget(self.play_new, 1, 2)
        pl.addWidget(self.play_from_snapshot, 2, 0, 1, 3)
        pl.addWidget(self.play_hint, 3, 0, 1, 3)
        self.play_old.clicked.connect(lambda: self.play('before'))
        self.play_new.clicked.connect(lambda: self.play('after'))
        self.play_from_snapshot.clicked.connect(self.play_selected)
        ll.addWidget(play)

        # ---- the snapshot library ----
        title = QLabel('Snapshot library')
        title.setStyleSheet('font-size: 16px; font-weight: bold')
        ll.addWidget(title)
        search_row = QHBoxLayout()
        self.search = QLineEdit()
        self.search.setPlaceholderText('search: stage, mission, checkpoint, enemy, object, name, notes ...')
        self.search.setClearButtonEnabled(True)
        self.search.textChanged.connect(lambda _t: self.refresh_checkpoints())
        self.count = QLabel()
        self.count.setStyleSheet('color: #888')
        search_row.addWidget(self.search, 1)
        search_row.addWidget(self.count)
        ll.addLayout(search_row)
        self.list = QListWidget()
        self.list.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.list.setIconSize(QSize(160, 90))
        self.list.setSpacing(2)
        self.list.setWordWrap(True)
        self.list.currentItemChanged.connect(lambda *_: self.refresh_details())
        self.list.itemDoubleClicked.connect(lambda _item: self.recreate_selected())
        ll.addWidget(self.list, 3)
        row = QHBoxLayout()
        for text, slot in (('New...', self.new_checkpoint), ('Rename...', self.rename), ('Delete', self.delete)):
            b = QPushButton(text)
            b.clicked.connect(slot)
            row.addWidget(b)
        ll.addLayout(row)
        row2 = QHBoxLayout()
        for text, slot in (('Make missing states', lambda: self.materialise_selected(None)),
                           ('Verify', self.verify), ('Re-read semantics', self.redescribe),
                           ('Details...', self.details_json)):
            b = QPushButton(text)
            b.clicked.connect(slot)
            row2.addWidget(b)
        ll.addLayout(row2)
        self.details = QTextBrowser()
        self.details.setOpenExternalLinks(False)
        ll.addWidget(self.details, 3)

        # ---- the selected snapshot's actions ----
        snap = QGroupBox('SELECTED SNAPSHOT')
        sg = QGridLayout(snap)
        self.snap_load_before = QPushButton()
        self.snap_load_after = QPushButton()
        self.snap_load_both = QPushButton('LOAD BOTH')
        self.render_reference = QPushButton('RENDER REFERENCE')
        self.render_native = QPushButton('RENDER NATIVE')
        self.snap_compare = QPushButton('PIXEL COMPARE')
        self.recreate = QPushButton('RECREATE EXACT FRAME')
        self.recreate.setStyleSheet('font-weight: bold; padding: 8px; font-size: 13px')
        for b in (self.snap_load_before, self.snap_load_after, self.snap_load_both, self.render_reference,
                  self.render_native, self.snap_compare):
            b.setStyleSheet(BIG)
        sg.addWidget(self.snap_load_before, 0, 0)
        sg.addWidget(self.snap_load_after, 0, 1)
        sg.addWidget(self.snap_load_both, 0, 2)
        sg.addWidget(self.render_reference, 1, 0)
        sg.addWidget(self.render_native, 1, 1)
        sg.addWidget(self.snap_compare, 1, 2)
        sg.addWidget(self.recreate, 0, 3, 2, 1)
        self.snap_load_before.clicked.connect(lambda: self.load_side('before'))
        self.snap_load_after.clicked.connect(lambda: self.load_side('after'))
        self.snap_load_both.clicked.connect(self.load_both)
        self.render_reference.clicked.connect(lambda: self.render_side('before'))
        self.render_native.clicked.connect(lambda: self.render_side('after'))
        self.snap_compare.clicked.connect(self.compare_selected)
        self.recreate.clicked.connect(self.recreate_selected)

        # ---- the two sides and the shared controls ----
        self.panels = {side: SidePanel(self, side) for side in SIDES}
        sides = QHBoxLayout()
        sides.addWidget(self.panels['before'], 1)
        sides.addWidget(self.panels['after'], 1)
        shared = QGroupBox('BOTH  (lockstep: the same game time on both sides)')
        sl = QGridLayout(shared)
        self.both_load = QPushButton('LOAD BOTH')
        self.both_reload = QPushButton('RELOAD BOTH')
        self.both_pause = QPushButton('PAUSE BOTH')
        self.both_resume = QPushButton('RESUME BOTH')
        self.both_stop = QPushButton('STOP BOTH')
        self.both_tick = QPushButton('STEP TICK')
        self.both_frame = QPushButton('STEP FRAME')
        self.both_10 = QPushButton('RUN 10')
        self.both_60 = QPushButton('RUN 60')
        self.both_n = QPushButton('RUN N')
        self.frames = QSpinBox()
        self.frames.setRange(1, 100000)
        self.frames.setValue(int(self.app.config.get('run_frames', 300)))
        self.frames.setSuffix(' frames')
        self.capture_runs = QCheckBox('capture the last frame')
        self.capture_runs.setChecked(True)
        self.both_capture = QPushButton('CAPTURE BOTH')
        self.both_diff = QPushButton('PIXEL DIFF')
        self.both_state = QPushButton('COMPARE STATE')
        for b in (self.both_load, self.both_reload, self.both_capture, self.both_diff):
            b.setStyleSheet(BIG)
        self.hold = QCheckBox('Hold loads on their first frame (lockstep), instead of running free')
        self.hold.setChecked(self.app.hold)
        self.hold.toggled.connect(self._hold_changed)
        self.lockstep = QLabel()
        self.lockstep.setStyleSheet('color: #3b82f6; font-weight: bold')
        controls = [(self.both_load, 0, 0), (self.both_reload, 0, 1), (self.both_pause, 0, 2),
                    (self.both_resume, 0, 3), (self.both_stop, 0, 4),
                    (self.both_tick, 1, 0), (self.both_frame, 1, 1), (self.both_10, 1, 2), (self.both_60, 1, 3),
                    (self.both_capture, 2, 0), (self.both_diff, 2, 1), (self.both_state, 2, 2)]
        for widget, r, c in controls:
            sl.addWidget(widget, r, c)
        nbox = QHBoxLayout()
        nbox.addWidget(self.frames)
        nbox.addWidget(self.both_n)
        sl.addLayout(nbox, 1, 4)
        sl.addWidget(self.capture_runs, 2, 3)
        sl.addWidget(self.hold, 3, 0, 1, 3)
        sl.addWidget(self.lockstep, 3, 3, 1, 2)
        self.both_load.clicked.connect(self.load_both)
        self.both_reload.clicked.connect(self.app.reload_both)
        self.both_pause.clicked.connect(lambda: [self.app.pause(s) for s in SIDES])
        self.both_resume.clicked.connect(lambda: [self.app.resume(s) for s in SIDES])
        self.both_stop.clicked.connect(lambda: [self.app.stop(s) for s in SIDES])
        self.both_tick.clicked.connect(lambda: self.app.step_tick(SIDES))
        self.both_frame.clicked.connect(lambda: self.run_both(1))
        self.both_10.clicked.connect(lambda: self.run_both(10))
        self.both_60.clicked.connect(lambda: self.run_both(60))
        self.both_n.clicked.connect(lambda: self.run_both(self.frames.value()))
        self.both_capture.clicked.connect(lambda: self.app.run_lockstep(SIDES, 2 * FIELD_TICKS, True, 'capture both'))
        self.both_diff.clicked.connect(self.pixel_diff)
        self.both_state.clicked.connect(lambda: self.app.request_states(SIDES))

        # ---- comparison, state, logs ----
        self.tabs = QTabWidget()
        self.compare = CompareView(self.app.config)
        self.state_table = QTableWidget(0, 5)
        self.state_table.setHorizontalHeaderLabels(['value', 'BEFORE', 'AFTER', 'same', 'meaning'])
        self.state_table.verticalHeader().setVisible(False)
        self.tool_log = LogView()
        self.logs = {side: LogView() for side in SIDES}
        self.tabs.addTab(self.compare, 'Compare captures')
        self.tabs.addTab(self.state_table, 'Compare state')
        self.tabs.addTab(self.tool_log, 'Log: tool')
        self.tabs.addTab(self.logs['before'], 'Log: BEFORE')
        self.tabs.addTab(self.logs['after'], 'Log: AFTER')

        right = QWidget()
        rl = QVBoxLayout(right)
        header = QLabel('Shadow Native Rewrite - Before / After Comparator')
        header.setStyleSheet('font-size: 18px; font-weight: bold')
        self.selected_label = QLabel()
        self.selected_label.setStyleSheet('font-size: 14px')
        self.selected_label.setWordWrap(True)
        self.selected_label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        rl.addWidget(header)
        rl.addWidget(self.selected_label)
        rl.addWidget(snap)
        rl.addLayout(sides)
        rl.addWidget(shared)
        splitter_v = QSplitter(Qt.Orientation.Vertical)
        top = QWidget()
        top.setLayout(rl)
        splitter_v.addWidget(top)
        splitter_v.addWidget(self.tabs)
        splitter_v.setSizes([560, 460])
        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(left)
        splitter.addWidget(splitter_v)
        splitter.setSizes([460, 1140])
        self.setCentralWidget(splitter)
        self.status_bar = self.statusBar()

        # ---- signals ----
        app.message.connect(self.tool_log.add)
        app.sessions_changed.connect(self.refresh_sessions)
        app.checkpoints_changed.connect(self.refresh_checkpoints)
        app.captures_ready.connect(self.show_captures)
        app.states_ready.connect(self.show_states)
        app.snapshot_created.connect(self.snapshot_created)
        app.recreated.connect(self.recreated)
        app.materialised.connect(lambda *_: self.refresh_checkpoints())
        app.live_info.connect(lambda side, info: self.panels[side].set_info(info))
        app.stages_ready.connect(self.fill_stages)
        self._log_hooks = {}
        self.timer = QTimer(self)
        self.timer.setInterval(1000)
        self.timer.timeout.connect(self._poll)
        self.timer.start()
        self.fill_stages(app.stages)
        self.refresh_checkpoints()
        self.refresh_sessions()
        self.tool_log.add(f'development link listening on {app.devlink.endpoint}; snapshot store {app.store.root}')
        app.load_stages()

    # ---- helpers ----
    def selected(self):
        item = self.list.currentItem()
        return self.app.store.get(item.data(Qt.ItemDataRole.UserRole)) if item is not None else None

    def _open(self, path):
        if hasattr(os, 'startfile'):
            os.startfile(str(path))

    def open_store(self):
        self._open(self.app.store.root)

    def open_selected(self):
        cp = self.selected()
        if cp is not None:
            self._open(cp.path)

    def _hold_changed(self, on):
        self.app.config['window']['hold'] = bool(on)
        self.app.save_config()

    def _poll(self):
        for side in SIDES:
            s = self.app.sessions.get(side)
            if s is not None and s.connection is not None and not self.app.running_lockstep:
                s.request('status')
        self.app.poll_live()
        jobs = self.app.busy_jobs()
        self.status_bar.showMessage(f'{jobs} headless job(s) running (replays, warm-ups, recreations)' if jobs else
                                    f'development link on {self.app.devlink.endpoint}; press '
                                    f'{self.app.snapshot_key} in a game window to snapshot the moment')

    # ---- PLAY ----
    def fill_stages(self, stages):
        current = self.play_stage.currentData()
        self.play_stage.blockSignals(True)
        self.play_stage.clear()
        entries = [(stg, name) for stg, name, bootable in stages if bootable] or \
                  [(stg, semantics.STAGE_TITLES.get(stg, stg)) for stg in ('stg0100', 'stg0200', 'stg0602', 'stg0604')]
        for stg, name in entries:
            self.play_stage.addItem(f'{name}  ({stg})', {'kind': 'boot-stage', 'stage': stg})
        self.play_stage.addItem('The story (from the title)', {'kind': 'boot-story'})
        self.play_stage.addItem('Cold boot (title screen)', {'kind': 'cold-boot'})
        for i in range(self.play_stage.count()):
            if self.play_stage.itemData(i) == current:
                self.play_stage.setCurrentIndex(i)
        self.play_stage.blockSignals(False)
        self.refresh_checkpoints()

    def play(self, side):
        route = self.play_stage.currentData() or {'kind': 'boot-stage', 'stage': 'stg0100'}
        try:
            self._hook_logs(self.app.play(side, dict(route)))
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Play', str(error))
            return
        self.tool_log.add(f"PLAY {self.app.side_label(side)}: {self.play_stage.currentText()} - press "
                          f"{self.app.snapshot_key} in its window to snapshot a moment")

    def play_selected(self):
        cp = self.selected()
        if cp is None:
            return
        side = 'before'
        if not cp.has_state(self.app.profile_of(side)):
            QMessageBox.information(self, 'Play', f"'{cp.name}' has no state for {self.app.side_label(side)} yet.")
            return
        try:
            self._hook_logs(self.app.play(side, {'kind': 'checkpoint', 'checkpoint': cp}))
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Play', str(error))

    # ---- the library ----
    def refresh_checkpoints(self):
        current = self.list.currentItem().data(Qt.ItemDataRole.UserRole) if self.list.currentItem() else None
        terms = [t for t in self.search.text().lower().split() if t]
        self.list.blockSignals(True)
        self.list.clear()
        before, after = self.app.profile_of('before'), self.app.profile_of('after')
        everything = sorted(self.app.store.list(), key=lambda c: c.meta.get('created', ''), reverse=True)
        shown = 0
        for cp in everything:
            if terms:
                text = semantics.search_text(cp.meta) + ' ' + cp.name.lower()
                if not all(t in text for t in terms):
                    continue
            shown += 1
            sem = cp.meta.get('semantics') or {}
            stage = sem.get('stage') or {}
            marks = []
            for pid in (before, after):
                state = cp.states.get(pid)
                if state is None or not cp.has_state(pid):
                    marks.append(f'{pid.upper()} -')
                elif state.get('verified'):
                    marks.append(f'{pid.upper()} OK')
                else:
                    marks.append(f'{pid.upper()} ?')
            where = ' / '.join(p for p in (stage.get('name'), (sem.get('mission') or {}).get('name'),
                                           f"CP{(sem.get('checkpoint') or {}).get('reached') or 0}"
                                           if stage.get('id') else None) if p)
            last = cp.meta.get('last_comparison')
            last_text = ''
            if last:
                stats = last.get('stats', {})
                last_text = ('\nOLD = NEW' if stats.get('identical') else
                             f"\nOLD vs NEW {stats.get('changed_percent', 0):.1f}% px")
            repro = cp.meta.get('reproduction')
            if repro:
                last_text += ' | replay EXACT' if repro.get('identical') else ' | replay differs'
            busy = '  (being made...)' if self.app.busy(cp.name) else ''
            made = (cp.meta.get('made_from') or {}).get('profile')
            # Short lines (the folder name is in the tooltip and the details).
            text = (f"{cp.title}\n{where or '-'}, {seconds(cp.saved_tick)}"
                    f"{(', by ' + made.upper()) if made else ''}\n{'  '.join(marks)}{busy}{last_text}")
            item = QListWidgetItem(text)
            item.setData(Qt.ItemDataRole.UserRole, cp.name)
            thumb = cp.path / 'thumb.png'
            item.setIcon(QIcon(str(thumb)) if thumb.exists() else placeholder_thumb())
            item.setToolTip(f"{cp.title}\n{cp.name}\n{cp.meta.get('description') or ''}\n{cp.describe_root()}")
            self.list.addItem(item)
            if cp.name == current:
                self.list.setCurrentItem(item)
        self.list.blockSignals(False)
        self.count.setText(f'{shown} of {len(everything)}' if terms else f'{len(everything)} snapshots')
        if self.list.currentItem() is None and self.list.count():
            self.list.setCurrentRow(0)
        self.refresh_details()

    def refresh_details(self):
        cp = self.selected()
        if cp is None:
            self.details.setHtml(f'<i>No snapshot yet. PLAY a build (above), play to any moment and press '
                                 f'{esc(self.app.snapshot_key)} in its window: the moment is saved, labeled and '
                                 f'listed here automatically. Or New... from a boot route.</i>')
            self.selected_label.setText('Selected: -')
            self.refresh_sessions()
            return
        self.selected_label.setText(f'Selected: <b>{esc(cp.title)}</b>')
        self.details.setHtml(self.details_html(cp))
        self.refresh_sessions()

    def details_html(self, cp) -> str:
        sem = cp.meta.get('semantics') or {}
        stage = sem.get('stage') or {}
        rows = [f"<h3>{esc(cp.title)}</h3><p style='color:#888'>{esc(cp.name)} &nbsp; ID "
                f"{esc(cp.meta.get('snapshot_id', '-'))} &nbsp; created {esc(cp.meta.get('created', ''))}</p>"]
        if cp.meta.get('description'):
            rows.append(f"<p>{esc(cp.meta['description'])}</p>")
        if (cp.path / 'thumb.png').exists():
            rows.append(f"<img src='{(cp.path / 'thumb.png').as_uri()}' width=320>")
        made = cp.meta.get('made_from') or {}
        live = cp.meta.get('live') or {}
        facts = []
        if stage.get('id'):
            mission = sem.get('mission') or {}
            cpi = sem.get('checkpoint') or {}
            facts.append(f"<b>Stage</b> {esc(stage.get('name'))} ({esc(stage.get('id'))}), <b>mission</b> "
                         f"{esc(mission.get('name'))}{' (accepted)' if mission.get('accepted') else ''}, "
                         f"<b>checkpoint</b> {esc(cpi.get('reached') or 0)} of {esc(cpi.get('savepoints'))}")
            if sem.get('area'):
                facts.append(f"<b>Area</b> {esc(sem['area'])}")
            if sem.get('event'):
                facts.append(f"<b>Event</b> {esc(sem['event'])}")
        else:
            facts.append('<b>Stage</b> none (a menu, a movie or a loading screen)')
        game = sem.get('game') or {}
        facts.append(f"<b>Game tick</b> {esc(cp.saved_tick)} ({seconds(cp.saved_tick)}); game frame "
                     f"{esc(game.get('frame_counter'))}; VI field {esc(live.get('vi_frames'))}; render frame "
                     f"{esc(live.get('render_frames'))}; recreated frame tick {esc(self.app.frame_tick(cp))}")
        player = sem.get('player')
        if player:
            facts.append(f"<b>Player</b> {vec(player.get('position'))}, heading {esc(player.get('heading_deg'))} deg, "
                         f"speed {esc(player.get('speed'))}, velocity {vec(player.get('velocity'))}")
        camera = sem.get('camera') or {}
        if camera.get('eye'):
            facts.append(f"<b>Camera</b> eye {vec(camera.get('eye'))}, yaw {esc(camera.get('yaw_deg'))} deg, pitch "
                         f"{esc(camera.get('pitch_deg'))} deg, hFOV {esc(camera.get('hfov_deg'))} deg, vFOV "
                         f"{esc(camera.get('vfov_deg'))} deg")
        entities = sem.get('entities') or {}
        if entities:
            layers = ', '.join(f"{esc(k)} {v['objects']}" for k, v in (entities.get('by_layer') or {}).items())
            classes = ', '.join(f"{esc(k)} {v}" for k, v in list((entities.get('by_class') or {}).items())[:10])
            facts.append(f"<b>Live objects</b> {entities.get('active', 0)} ({layers}); {classes}")
        if sem.get('nearby'):
            facts.append('<b>Nearby</b> ' + ', '.join(
                f"{esc(n['type'])} #{n['record']} {n['distance']:.0f}{' (live)' if n['live'] else ''}"
                for n in sem['nearby'][:6]))
        facts.append(f"<b>Rings</b> {esc(game.get('rings'))}, <b>score</b> {esc(game.get('score'))}, <b>RNG</b> "
                     f"{esc(game.get('rng_sha'))}")
        if made:
            facts.append(f"<b>Made</b> by {esc((made.get('profile') or '').upper())} live ({esc(made.get('start'))}"
                         f"{'' if made.get('root_valid', True) else '; its start was replaced'})")
        facts.append(f"<b>Recipe</b> {esc(cp.describe_root())}; boundary asked at tick "
                     f"{esc(cp.recipe.get('stop_tick'))}; warm-up from tick {esc(cp.recipe.get('warmup_tick'))}; "
                     f"display {esc(cp.recipe.get('display'))}")
        rows.append('<p>' + '<br>'.join(facts) + '</p>')
        verification = cp.meta.get('verification', {})
        if verification:
            rows.append(f"<p><b>Builds:</b> {esc(verification.get('summary', ''))}</p>")
        rows.append('<table cellspacing=4><tr><th align=left>build</th><th align=left>state</th>'
                    '<th align=left>made</th><th align=left>check</th><th align=left>warm-up</th></tr>')
        for pid in self.app.config['profiles']:
            st = cp.states.get(pid)
            if st is None:
                rows.append(f'<tr><td>{pid.upper()}</td><td colspan=4><i>not made yet</i></td></tr>')
                continue
            check = verification.get('profiles', {}).get(pid, {})
            exe_now = self.app.config['profiles'][pid]['exe']
            stale = ''
            if st.get('exe_sha256') and file_sha256(Path(exe_now)) != st.get('exe_sha256'):
                stale = ' (made by an older executable)'
            replay = st.get('replay_check')
            replay_text = ''
            if replay:
                replay_text = ('; replay = live' if replay.get('ok') else
                               f"; replay DIFFERS ({replay.get('memory_blocks_differing')} blocks)")
            warm = st.get('warmup') or {}
            rows.append(f"<tr><td>{pid.upper()}</td><td>v{st.get('savestate_version')} kind "
                        f"{st.get('renderer_kind')}{stale}</td><td>{esc(st.get('method'))} {esc(st.get('created', ''))}"
                        f"</td><td>{'OK' if st.get('verified') else 'DIFFERS' if check else '?'}"
                        f"{'; memory blocks differing ' + str(check.get('memory')) if check.get('memory') else ''}"
                        f"{replay_text}</td><td>{esc(warm.get('game_ticks')) if warm else '-'}</td></tr>")
        rows.append('</table>')
        ref = cp.meta.get('reference')
        if ref:
            rows.append(f"<p><b>Reference frame</b> (what {esc(ref.get('profile', '').upper())} showed): frame "
                        f"{esc(ref.get('frame'))}, game tick {esc(ref.get('frame_game_ticks'))}, "
                        f"{esc(ref.get('size'))}</p>")
        repro = cp.meta.get('reproduction')
        if repro:
            rows.append(f"<p><b>Replay vs live</b> ({esc(repro.get('profile', '').upper())}): "
                        f"{'EXACT' if repro.get('identical') else esc(imagediff.describe(repro.get('stats', {})))}</p>")
        last = cp.meta.get('last_comparison')
        if last:
            stats = last.get('stats', {})
            rows.append(f"<p><b>Last comparison</b> {esc(last.get('when'))}: {esc(last.get('label'))}, same frame "
                        f"{'yes' if last.get('same_frame') else 'NO'}; {esc(imagediff.describe(stats))}</p>")
        return ''.join(rows)

    def snapshot_created(self, name, side):
        self.search.blockSignals(True)
        self.search.clear()
        self.search.blockSignals(False)
        self.refresh_checkpoints()
        for i in range(self.list.count()):
            if self.list.item(i).data(Qt.ItemDataRole.UserRole) == name:
                self.list.setCurrentRow(i)
                break
        self.status_bar.showMessage(f'[Snapshot] saved as {name} ({self.app.side_label(side)})', 8000)

    def recreated(self, payload):
        self.refresh_checkpoints()

    def create_snapshot(self, side):
        if not self.app.connected(side):
            QMessageBox.information(self, 'Create Snapshot', f'No game is running on the {side} side.')
            return
        try:
            self.app.create_snapshot(side)
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Create Snapshot', str(error))

    def new_checkpoint(self, session_side=None):
        dialog = NewCheckpointDialog(self.app, self.selected(), session_side, self)
        if not dialog.exec():
            return
        spec = dialog.result_spec()
        try:
            if spec['mode'] == 'route':
                self.app.new_checkpoint_from_route(spec['name'], spec['route'], spec['seconds'], spec['description'])
            elif spec['mode'] == 'session':
                self.app.create_snapshot(spec['side'], spec['name'], spec['description'])
            else:
                self.app.derive_checkpoint(spec['parent'], spec['name'], spec['seconds'], spec['description'])
        except Exception as error:  # noqa: BLE001 - shown to the user
            QMessageBox.critical(self, 'New snapshot', str(error))
            return
        self.refresh_checkpoints()

    def rename(self):
        cp = self.selected()
        if cp is None:
            return
        dialog = RenameDialog(cp, self)
        if not dialog.exec():
            return
        name, title, description = dialog.values()
        if name != cp.name and not valid_name(name):
            QMessageBox.warning(self, 'Rename', 'The folder name: letters, digits, _ - . (up to 100).')
            return
        try:
            self.app.rename(cp.name, name, description, title)
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Rename', str(error))
        self.refresh_checkpoints()

    def delete(self):
        cp = self.selected()
        if cp is None:
            return
        if self.app.busy(cp.name):
            QMessageBox.information(self, 'Delete', f"'{cp.name}' is still being made; try again when it is done.")
            return
        children = self.app.store.children(cp.name)
        extra = (f'\n\n{len(children)} snapshot(s) start from it and could no longer be made again: '
                 + ', '.join(c.name for c in children)) if children else ''
        if QMessageBox.question(self, 'Delete snapshot',
                                f"Move '{cp.name}' to the store's .trash folder?{extra}") != \
                QMessageBox.StandardButton.Yes:
            return
        where = self.app.store.delete(cp.name)
        self.tool_log.add(f"moved '{cp.name}' to {where}")
        self.refresh_checkpoints()

    def materialise_selected(self, side):
        cp = self.selected()
        if cp is None:
            return
        if side is None:
            self.app.complete_snapshot(cp) if cp.meta.get('kind') == 'one-button' else self.app.materialise_all(cp)
        else:
            self.app.materialise(cp, self.app.profile_of(side))

    def redescribe(self):
        cp = self.selected()
        if cp is not None:
            self.app.redescribe(cp)

    def verify(self):
        cp = self.selected()
        if cp is None:
            return
        self.app.verify(cp)
        cp.save()
        self.refresh_checkpoints()
        before, after = self.app.profile_of('before'), self.app.profile_of('after')
        if cp.report(before) and cp.report(after):
            self.show_states({'before': cp.report(before), 'after': cp.report(after)}, label=f'{cp.name} (saved)')
            self.tabs.setCurrentWidget(self.state_table)

    def details_json(self):
        cp = self.selected()
        if cp is not None:
            show_json(self, cp.name, cp.meta)

    # ---- the selected snapshot ----
    def recreate_selected(self):
        cp = self.selected()
        if cp is not None:
            self.app.recreate(cp, SIDES)
            self.tabs.setCurrentWidget(self.tool_log)

    def render_side(self, side):
        cp = self.selected()
        if cp is not None:
            label = 'RENDER REFERENCE' if side == 'before' else 'RENDER NATIVE'
            self.app.recreate(cp, [side], label=label)
            self.tabs.setCurrentWidget(self.tool_log)

    def compare_selected(self):
        cp = self.selected()
        if cp is None:
            return
        pair = self.app.latest_pair(cp)
        if not pair.get('before') or not pair.get('after'):
            QMessageBox.information(self, 'Pixel compare', 'Render both builds first (RECREATE EXACT FRAME, or '
                                    'RENDER REFERENCE and RENDER NATIVE).')
            return
        self.compare.set_captures(pair['before'], pair['after'], f'{cp.title}: the latest renders')
        self.compare.mode.setCurrentIndex(MODES.index('Difference'))
        self.tabs.setCurrentWidget(self.compare)

    # ---- sessions ----
    def load_side(self, side):
        cp = self.selected()
        if cp is None:
            return
        try:
            self._hook_logs(self.app.load(side, cp))
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Load', str(error))

    def load_both(self):
        cp = self.selected()
        if cp is None:
            return
        missing = [self.app.profile_of(s) for s in SIDES if not cp.has_state(self.app.profile_of(s))]
        if missing:
            if QMessageBox.question(self, 'Load both', f"'{cp.name}' has no state yet for "
                                    f"{', '.join(m.upper() for m in missing)}. Make it now (headless replay)?") == \
                    QMessageBox.StandardButton.Yes:
                self.app.materialise_all(cp)
            return
        try:
            self.app.load_both(cp)
            for side in SIDES:
                self._hook_logs(self.app.sessions[side])
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Load both', str(error))

    def launch_side_route(self, side):
        stages = [f'{stg}  {name}' for stg, name, bootable in self.app.stages if bootable] or ['stg0100', 'stg0604']
        choice, ok = QInputDialog.getItem(self, 'Play from a route', 'Boot straight into', ['(cold boot)', '(story)'] +
                                          stages, 2 if len(stages) else 0, False)
        if not ok:
            return
        if choice == '(cold boot)':
            route = {'kind': 'cold-boot'}
        elif choice == '(story)':
            route = {'kind': 'boot-story'}
        else:
            route = {'kind': 'boot-stage', 'stage': choice.split()[0]}
        try:
            self._hook_logs(self.app.play(side, route))
        except Exception as error:  # noqa: BLE001
            QMessageBox.critical(self, 'Play', str(error))

    def run_both(self, frames: int):
        self.app.run_lockstep(SIDES, frames * FIELD_TICKS, self.capture_runs.isChecked(),
                              f'run {frames} frame{"s" if frames != 1 else ""}')

    def pixel_diff(self):
        self.compare.mode.setCurrentIndex(MODES.index('Difference'))
        self.tabs.setCurrentWidget(self.compare)

    def _hook_logs(self, session):
        if session is None or session in self._log_hooks:
            return
        self._log_hooks[session] = True
        session.log.connect(self.logs[session.side].add)

    def refresh_sessions(self):
        for panel in self.panels.values():
            panel.refresh()
        ls = self.app.lockstep
        self.lockstep.setText(f"LOCKSTEP: {ls['checkpoint']} + {ls['ticks'] / TICKS_PER_SECOND:.4f} s of game time"
                              if ls else 'not in lockstep')
        both = all(self.app.connected(s) for s in SIDES)
        for b in (self.both_reload, self.both_pause, self.both_resume, self.both_tick, self.both_frame, self.both_10,
                  self.both_60, self.both_n, self.both_capture, self.both_state, self.both_stop):
            b.setEnabled(both)
        cp = self.selected()
        before, after = self.app.profile_of('before'), self.app.profile_of('after')
        self.both_load.setEnabled(cp is not None)
        self.snap_load_before.setText(f'LOAD {before.upper()}')
        self.snap_load_after.setText(f'LOAD {after.upper()}')
        self.render_reference.setText(f'RENDER REFERENCE ({before.upper()})')
        self.render_native.setText(f'RENDER NATIVE ({after.upper()})')
        self.snap_load_before.setEnabled(cp is not None and cp.has_state(before))
        self.snap_load_after.setEnabled(cp is not None and cp.has_state(after))
        self.snap_load_both.setEnabled(cp is not None)
        for b in (self.render_reference, self.render_native, self.recreate, self.snap_compare):
            b.setEnabled(cp is not None)
        self.play_from_snapshot.setEnabled(cp is not None and cp.has_state(before))
        self.play_old.setText(f'PLAY {before.upper()} (reference)')
        self.play_new.setText(f'PLAY {after.upper()}')
        self.play_hint.setText(f"Plays normally (live input, no lockstep). Press {self.app.snapshot_key} in the game "
                               'window at any moment inside a stage: that exact moment is saved, labeled from what '
                               'the game state is (stage, mission, checkpoint, player, camera, nearby objects) and '
                               'listed below; the other build is brought to it in the background, and its frame is '
                               'recreated in both and compared.')

    # ---- results ----
    def show_captures(self, payload):
        before, after = payload.get('before'), payload.get('after')
        if before is None and after is None:
            reply = payload.get('before_reply') or payload.get('after_reply') or {}
            self.tool_log.add(f"no capture written ({reply.get('error', '')})")
            return
        label = payload.get('label', '')
        if 'same_frame' in payload:
            ticks = payload.get('frame_ticks') or {}
            label += ('   same frame (tick %s)' % ticks.get('before') if payload.get('same_frame') else
                      f"   DIFFERENT FRAMES (ticks {ticks.get('before')} / {ticks.get('after')})")
        self.compare.set_captures(before, after, label)
        self.tabs.setCurrentWidget(self.compare)
        self.tool_log.add(f"captures {payload.get('label')}: BEFORE {before}  AFTER {after}")
        self.refresh_checkpoints()

    def show_states(self, reports, label=''):
        before, after = reports.get('before'), reports.get('after')
        rows = probes.compare_reports(before or {}, after or {})
        self.state_table.setRowCount(len(rows))
        for i, (name, a, b, same, note) in enumerate(rows):
            values = [name, probes.format_value(a), probes.format_value(b),
                      '' if same is None else ('yes' if same else 'NO'), note]
            for j, text in enumerate(values):
                item = QTableWidgetItem(str(text))
                if j == 3 and same is False:
                    item.setForeground(QColor('#dc2626'))
                self.state_table.setItem(i, j, item)
        self.state_table.resizeColumnsToContents()
        self.state_table.horizontalHeader().setSectionResizeMode(4, QHeaderView.ResizeMode.Stretch)
        if label:
            self.tool_log.add(f'state compared: {label}')

    # ---- misc ----
    def settings(self):
        if SettingsDialog(self.app, self).exec():
            self.tool_log.add('settings saved (the snapshot store path applies at the next start; the snapshot key '
                              'at the next game launch)')
            self.refresh_sessions()

    def help(self):
        QMessageBox.information(self, 'How it works', HELP_TEXT.replace('{KEY}', self.app.snapshot_key))

    def closeEvent(self, event):
        running = [s for s in SIDES if self.app.sessions.get(s) is not None and self.app.sessions[s].running()]
        if running and QMessageBox.question(self, 'Quit', 'Stop the running game(s) and quit?') != \
                QMessageBox.StandardButton.Yes:
            event.ignore()
            return
        self.app.shutdown()
        event.accept()


HELP_TEXT = """ONE BUTTON: PLAY a build (the reference build, OLD, by default) from a stage, play normally, and press {KEY}
in the game window at any moment. The game saves that exact moment at its next simulation boundary - its own
savestate, the input you played since it started, a state report and the next frame (what was on screen) - and the
workstation adds it to the library at once, named and described from the game state itself: the stage, the
mission, the checkpoint reached, the player's position and heading, the camera, the live objects, the nearest stage
objects and what is happening (an enemy encounter, a boss, at a checkpoint), the random generator, the game tick.
Rename it any time (Rename...); search the library by any of it.

In the background the OTHER build is brought to the same moment by replaying exactly the input you played (both
are checked to agree: clocks, the probes, the random generator, the player, the camera), both builds save a
warm-up state a second before it, and the frame is RECREATED in both and compared: a snapshot is a cross-renderer
reproduction of a visual moment, not just a savestate.

RECREATE EXACT FRAME renders the snapshot's frame again in both builds, headless: each loads its own warm-up state,
replays the input to the snapshot, and captures the frame with the game tick the live frame had (the renderers'
frame-to-frame history - motion trails, the radial blur's saved screen - is rebuilt on the way). The two renders are
compared pixel by pixel; the reference build's render is also compared with the frame that was on screen (replay
EXACT = the reproduction is the moment you saw). RENDER REFERENCE / RENDER NATIVE render one build; PIXEL COMPARE
shows the latest pair as a difference image.

LOAD OLD / LOAD NEW / LOAD BOTH restore the selected snapshot in game windows (in the running game when there is
one - fast - else in a new window). RELOAD BOTH returns to it again. STEP TICK advances to the next simulation
boundary, STEP FRAME one display field (1/59.94 s) of game time, RUN 10 / 60 / N that many; both sides pause at
the same boundary.

Hotkeys in a game window: {KEY} snapshot (Settings), N / M save / load the game's own quick slot (M replaces the
start: snapshots made after it exist for that build only)."""
