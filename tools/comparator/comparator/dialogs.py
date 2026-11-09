"""Dialogs: a new checkpoint, renaming one, the settings."""
from __future__ import annotations

import json
import os
from pathlib import Path

from PySide6.QtWidgets import (QButtonGroup, QCheckBox, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox,
                               QFileDialog, QFormLayout, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
                               QMessageBox, QPlainTextEdit, QPushButton, QRadioButton, QSpinBox, QVBoxLayout,
                               QWidget)

from .checkpoints import valid_name


class NewCheckpointDialog(QDialog):
    """A checkpoint from a boot route, from a running session, or derived from the selected checkpoint."""

    def __init__(self, app, selected=None, session_side=None, parent=None):
        super().__init__(parent)
        self.app = app
        self.setWindowTitle('New comparison checkpoint')
        self.setMinimumWidth(560)
        layout = QVBoxLayout(self)

        form = QFormLayout()
        self.name = QLineEdit()
        self.name.setPlaceholderText('e.g. westopolis_start, before_bigfoot, final_haunt_glare')
        self.description = QLineEdit()
        form.addRow('Name', self.name)
        form.addRow('Description', self.description)
        layout.addLayout(form)

        self.group = QButtonGroup(self)
        # ---- from a boot route ----
        self.route_radio = QRadioButton('From a boot route (both builds replay it headless)')
        route_box = QGroupBox()
        rl = QGridLayout(route_box)
        self.route_kind = QComboBox()
        self.route_kind.addItems(['Boot into a stage', 'Boot into the story', 'Cold boot (title screen)'])
        self.stage = QComboBox()
        self.stage.setEditable(True)
        self.fill_stages(app.stages)
        app.stages_ready.connect(self.fill_stages)
        self.seconds = QDoubleSpinBox()
        self.seconds.setRange(0.1, 3600.0)
        self.seconds.setDecimals(2)
        self.seconds.setValue(15.0)
        self.seconds.setSuffix(' s of game time')
        self.script = QLineEdit()
        self.script.setPlaceholderText('optional scripted input (.uitest), e.g. source/tools/ui-test/example.uitest')
        browse = QPushButton('...')
        browse.clicked.connect(self.pick_script)
        rl.addWidget(QLabel('Route'), 0, 0)
        rl.addWidget(self.route_kind, 0, 1)
        rl.addWidget(QLabel('Stage'), 1, 0)
        rl.addWidget(self.stage, 1, 1)
        rl.addWidget(QLabel('Stop at'), 2, 0)
        rl.addWidget(self.seconds, 2, 1)
        rl.addWidget(QLabel('Script'), 3, 0)
        sl = QHBoxLayout()
        sl.addWidget(self.script)
        sl.addWidget(browse)
        rl.addLayout(sl, 3, 1)
        self.route_kind.currentIndexChanged.connect(lambda i: self.stage.setEnabled(i == 0))
        layout.addWidget(self.route_radio)
        layout.addWidget(route_box)

        # ---- from a running session ----
        self.session_radio = QRadioButton('From a running game (saved now; the other build replays its input)')
        session_box = QGroupBox()
        sll = QHBoxLayout(session_box)
        self.session_side = QComboBox()
        for side in ('before', 'after'):
            s = app.sessions.get(side)
            if s is not None and s.connection is not None:
                self.session_side.addItem(f"{side.upper()} ({s.profile_id}, from {s.start_label()})", side)
        sll.addWidget(QLabel('Session'))
        sll.addWidget(self.session_side, 1)
        layout.addWidget(self.session_radio)
        layout.addWidget(session_box)

        # ---- derived ----
        self.derive_radio = QRadioButton('Derived: the selected checkpoint + some seconds, no input')
        derive_box = QGroupBox()
        dl = QHBoxLayout(derive_box)
        self.parent_name = selected.name if selected is not None else ''
        self.derive_seconds = QDoubleSpinBox()
        self.derive_seconds.setRange(0.1, 3600.0)
        self.derive_seconds.setValue(5.0)
        self.derive_seconds.setSuffix(' s later')
        dl.addWidget(QLabel(f"From '{self.parent_name}'" if self.parent_name else 'Select a checkpoint first'))
        dl.addWidget(self.derive_seconds)
        layout.addWidget(self.derive_radio)
        layout.addWidget(derive_box)

        for i, radio in enumerate((self.route_radio, self.session_radio, self.derive_radio)):
            self.group.addButton(radio, i)
        self.session_radio.setEnabled(self.session_side.count() > 0)
        self.derive_radio.setEnabled(bool(self.parent_name) and selected is not None and
                                     selected.saved_tick is not None)
        if session_side is not None and self.session_radio.isEnabled():
            self.session_radio.setChecked(True)
            index = self.session_side.findData(session_side)
            if index >= 0:
                self.session_side.setCurrentIndex(index)
        else:
            self.route_radio.setChecked(True)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.check)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def fill_stages(self, stages):
        current = self.stage.currentText()
        self.stage.clear()
        for stg, name, bootable in stages:
            if bootable:
                self.stage.addItem(f'{stg}  {name}', stg)
        if self.stage.count() == 0:
            for stg in ('stg0100', 'stg0604', 'stg0602', 'stg0404'):
                self.stage.addItem(stg, stg)
        if current:
            index = self.stage.findText(current)
            if index >= 0:
                self.stage.setCurrentIndex(index)

    def pick_script(self):
        path, _ = QFileDialog.getOpenFileName(self, 'Scripted input', '', 'UI test scripts (*.uitest);;All (*)')
        if path:
            self.script.setText(path)

    def check(self):
        if not valid_name(self.name.text().strip()):
            QMessageBox.warning(self, 'Name', 'Use letters, digits, _ - . (up to 100), starting with a letter or digit.')
            return
        if self.app.store.get(self.name.text().strip()) is not None:
            QMessageBox.warning(self, 'Name', 'A checkpoint with this name exists.')
            return
        self.accept()

    def result_spec(self) -> dict:
        spec = {'name': self.name.text().strip(), 'description': self.description.text().strip()}
        mode = self.group.checkedId()
        if mode == 0:
            kinds = ('boot-stage', 'boot-story', 'cold-boot')
            route = {'kind': kinds[self.route_kind.currentIndex()]}
            if route['kind'] == 'boot-stage':
                route['stage'] = self.stage.currentData() or self.stage.currentText().split()[0]
            if self.script.text().strip():
                route['ui_test_script'] = self.script.text().strip()
            spec.update(mode='route', route=route, seconds=self.seconds.value())
        elif mode == 1:
            spec.update(mode='session', side=self.session_side.currentData())
        else:
            spec.update(mode='derive', parent=self.parent_name, seconds=self.derive_seconds.value())
        return spec


class RenameDialog(QDialog):
    """A snapshot's folder name, its readable title and notes (a one-button snapshot gets all from the game state;
    any can be changed)."""

    def __init__(self, cp, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Rename snapshot')
        self.setMinimumWidth(760)
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.name = QLineEdit(cp.name)
        self.title = QLineEdit(cp.title)
        self.description = QLineEdit(cp.meta.get('description', ''))
        self.description.setPlaceholderText('optional notes: what is interesting here')
        form.addRow('Name (folder)', self.name)
        form.addRow('Title', self.title)
        form.addRow('Notes', self.description)
        layout.addLayout(form)
        auto = cp.meta.get('auto_name')
        if auto:
            hint = QLabel(f'Automatic name: {auto}')
            hint.setStyleSheet('color: #888')
            layout.addWidget(hint)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def values(self):
        return self.name.text().strip(), self.title.text().strip(), self.description.text().strip()


class SettingsDialog(QDialog):
    """The main settings as a form; everything else in the JSON file itself."""

    def __init__(self, app, parent=None):
        super().__init__(parent)
        self.app = app
        self.setWindowTitle('Comparator settings')
        self.setMinimumWidth(760)
        cfg = app.config
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.game_data = self._path_row(form, 'Game data (--game-data)', cfg['game_data'], directory=True)
        self.states_dir = self._path_row(form, 'Checkpoint store', cfg['states_dir'], directory=True)
        self.before = QComboBox()
        self.after = QComboBox()
        for pid in cfg['profiles']:
            self.before.addItem(pid)
            self.after.addItem(pid)
        self.before.setCurrentText(cfg['before'])
        self.after.setCurrentText(cfg['after'])
        form.addRow('BEFORE (left) profile', self.before)
        form.addRow('AFTER (right) profile', self.after)
        self.exes = {}
        self.args = {}
        for pid, prof in cfg['profiles'].items():
            self.exes[pid] = self._path_row(form, f"'{pid}' executable", prof['exe'], directory=False)
            line = QLineEdit(' '.join(prof.get('args', [])))
            form.addRow(f"'{pid}' extra arguments", line)
            self.args[pid] = line
        win = cfg['window']
        self.width = QSpinBox()
        self.width.setRange(320, 7680)
        self.width.setValue(int(win['width']))
        self.height = QSpinBox()
        self.height.setRange(180, 4320)
        self.height.setValue(int(win['height']))
        size = QHBoxLayout()
        size.addWidget(self.width)
        size.addWidget(QLabel('x'))
        size.addWidget(self.height)
        size_widget = QWidget()
        size_widget.setLayout(size)
        form.addRow('Window client size (both)', size_widget)
        self.audio = QComboBox()
        self.audio.addItems(['after', 'before', 'both', 'none'])
        self.audio.setCurrentText(win.get('audio', 'after'))
        form.addRow('Audio from', self.audio)
        self.input = QComboBox()
        self.input.addItems(['both', 'before', 'after', 'none'])
        self.input.setCurrentText(win.get('input', 'both'))
        form.addRow('Gamepad (XInput) to', self.input)
        self.side_by_side = QCheckBox('Place BEFORE left and AFTER right')
        self.side_by_side.setChecked(bool(win.get('side_by_side', True)))
        form.addRow('Windows', self.side_by_side)
        self.snapshot_key = QComboBox()
        self.snapshot_key.setEditable(True)
        self.snapshot_key.addItems([f'F{i}' for i in range(1, 13)] + ['none'])
        self.snapshot_key.setCurrentText(cfg.get('snapshot_hotkey') or 'F5')
        form.addRow('Snapshot key (in the game window)', self.snapshot_key)
        recreate = cfg.get('recreate') or {}
        self.warmup = QSpinBox()
        self.warmup.setRange(0, 3600)
        self.warmup.setValue(int(recreate.get('warmup_fields', 60)))
        self.warmup.setSuffix(' fields before the snapshot (0 = from the snapshot itself)')
        form.addRow('Recreation warm-up', self.warmup)
        self.auto_recreate = QCheckBox('Recreate and compare every new snapshot automatically')
        self.auto_recreate.setChecked(bool(recreate.get('auto', True)))
        form.addRow('Recreation', self.auto_recreate)
        layout.addLayout(form)
        raw = QPushButton('Open the JSON file (all settings, profile env / args)...')
        raw.clicked.connect(lambda: os.startfile(str(app.config_path)) if hasattr(os, 'startfile') else None)
        layout.addWidget(raw)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.save)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _path_row(self, form, label, value, directory):
        line = QLineEdit(str(value))
        button = QPushButton('...')

        def pick():
            if directory:
                path = QFileDialog.getExistingDirectory(self, label, line.text())
            else:
                path, _ = QFileDialog.getOpenFileName(self, label, line.text(), 'Executables (*.exe);;All (*)')
            if path:
                line.setText(str(Path(path)))

        button.clicked.connect(pick)
        row = QHBoxLayout()
        row.addWidget(line, 1)
        row.addWidget(button)
        widget = QWidget()
        widget.setLayout(row)
        form.addRow(label, widget)
        return line

    def save(self):
        cfg = self.app.config
        cfg['game_data'] = self.game_data.text().strip()
        cfg['states_dir'] = self.states_dir.text().strip()
        cfg['before'] = self.before.currentText()
        cfg['after'] = self.after.currentText()
        for pid, line in self.exes.items():
            cfg['profiles'][pid]['exe'] = line.text().strip()
            cfg['profiles'][pid]['args'] = self.args[pid].text().split()
        cfg['window'].update(width=self.width.value(), height=self.height.value(), audio=self.audio.currentText(),
                             input=self.input.currentText(), side_by_side=self.side_by_side.isChecked())
        key = self.snapshot_key.currentText().strip()
        if not (key.lower() == 'none' or (len(key) == 1 and key.isalnum()) or
                (key[:1].upper() == 'F' and key[1:].isdigit() and 1 <= int(key[1:]) <= 24)):
            QMessageBox.warning(self, 'Snapshot key', 'F1..F24, one letter or digit, or none.')
            return
        cfg['snapshot_hotkey'] = key.upper() if key.lower() != 'none' else 'none'
        cfg.setdefault('recreate', {}).update(warmup_fields=self.warmup.value(),
                                              auto=self.auto_recreate.isChecked())
        self.app.save_config()
        self.accept()


def show_json(parent, title, data):
    dialog = QDialog(parent)
    dialog.setWindowTitle(title)
    dialog.resize(800, 600)
    layout = QVBoxLayout(dialog)
    text = QPlainTextEdit(json.dumps(data, indent=2))
    text.setReadOnly(True)
    layout.addWidget(text)
    dialog.exec()
