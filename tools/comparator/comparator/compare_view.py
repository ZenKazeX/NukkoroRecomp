"""The capture comparison widget: side by side, overlay with an opacity slider, blink, the difference image and its
statistics, BEFORE or AFTER alone."""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PySide6.QtCore import QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QImage, QPainter, QPen
from PySide6.QtWidgets import (QCheckBox, QComboBox, QFileDialog, QHBoxLayout, QLabel, QPushButton, QSizePolicy,
                               QSlider, QSpinBox, QVBoxLayout, QWidget)

from . import imagediff

MODES = ('Side by side', 'Overlay', 'Blink', 'Difference', 'BEFORE only', 'AFTER only')


def to_qimage(array: np.ndarray) -> QImage:
    array = np.ascontiguousarray(array)
    h, w = array.shape[:2]
    return QImage(array.data, w, h, 3 * w, QImage.Format.Format_RGB888).copy()


class ImageCanvas(QWidget):
    """Paints one image scaled to fit (aspect kept), with a caption."""
    clicked = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.image = None
        self.caption = ''
        self.setMinimumSize(320, 120)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_image(self, image: QImage, caption: str = ''):
        self.image = image
        self.caption = caption
        self.update()

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(18, 18, 22))
        if self.image is None or self.image.isNull():
            painter.setPen(QColor(150, 150, 160))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter,
                             'No captures yet - Capture BOTH or Run N frames with capture')
            return
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        iw, ih = self.image.width(), self.image.height()
        scale = min(self.width() / iw, (self.height() - 18) / ih)
        w, h = iw * scale, ih * scale
        x = (self.width() - w) / 2
        y = (self.height() - 18 - h) / 2
        painter.drawImage(QRectF(x, y, w, h), self.image)
        if self.caption:
            # One line inside the canvas: a long caption is elided rather than spilling over the stats below.
            painter.setPen(QPen(QColor(220, 220, 230)))
            text = painter.fontMetrics().elidedText(self.caption, Qt.TextElideMode.ElideMiddle,
                                                    max(40, self.width() - 8))
            painter.drawText(QRectF(0, self.height() - 18, self.width(), 18), Qt.AlignmentFlag.AlignCenter, text)

    def mousePressEvent(self, _event):
        self.clicked.emit()


class CompareView(QWidget):
    def __init__(self, config: dict, parent=None):
        super().__init__(parent)
        self.config = config
        self.before = None      # numpy RGB as captured
        self.after = None
        self.before_path = None
        self.after_path = None
        self.label = ''
        self.blink_state = False

        bar = QHBoxLayout()
        self.mode = QComboBox()
        self.mode.addItems(MODES)
        self.mode.currentIndexChanged.connect(self.refresh)
        bar.addWidget(QLabel('View'))
        bar.addWidget(self.mode)
        self.opacity = QSlider(Qt.Orientation.Horizontal)
        self.opacity.setRange(0, 100)
        self.opacity.setValue(50)
        self.opacity.setFixedWidth(160)
        self.opacity.valueChanged.connect(self.refresh)
        self.opacity_label = QLabel('AFTER 50%')
        bar.addWidget(QLabel('Overlay'))
        bar.addWidget(self.opacity)
        bar.addWidget(self.opacity_label)
        self.blink_ms = QSpinBox()
        self.blink_ms.setRange(100, 3000)
        self.blink_ms.setSingleStep(100)
        self.blink_ms.setValue(500)
        self.blink_ms.setSuffix(' ms')
        self.blink_ms.valueChanged.connect(lambda v: self.timer.setInterval(v))
        bar.addWidget(QLabel('Blink'))
        bar.addWidget(self.blink_ms)
        self.amplify = QSpinBox()
        self.amplify.setRange(1, 64)
        self.amplify.setValue(int(config['capture'].get('amplify', 4)))
        self.amplify.setPrefix('x')
        self.amplify.valueChanged.connect(self.refresh)
        self.threshold = QSpinBox()
        self.threshold.setRange(0, 255)
        self.threshold.setValue(int(config['capture'].get('diff_threshold', 8)))
        self.threshold.valueChanged.connect(self.refresh)
        bar.addWidget(QLabel('Diff gain'))
        bar.addWidget(self.amplify)
        bar.addWidget(QLabel('threshold'))
        bar.addWidget(self.threshold)
        self.fit = QCheckBox('Fit BEFORE to AFTER')
        self.fit.setToolTip('The legacy renderer captures its 4:3 EFB; its presenter stretched it to the window. '
                            'Fitting shows what that build displayed.')
        self.fit.setChecked(bool(config['capture'].get('fit_before_to_after', True)))
        self.fit.toggled.connect(self.refresh)
        bar.addWidget(self.fit)
        bar.addStretch(1)
        open_button = QPushButton('Open images...')
        open_button.clicked.connect(self.open_images)
        save_button = QPushButton('Save view...')
        save_button.clicked.connect(self.save_view)
        bar.addWidget(open_button)
        bar.addWidget(save_button)

        self.canvas = ImageCanvas()
        self.canvas.clicked.connect(self._toggle_blink_manually)
        self.stats = QLabel('')
        self.stats.setWordWrap(True)
        self.stats.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.stats.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.stats.setFixedHeight(3 * self.stats.fontMetrics().lineSpacing() + 4)
        layout = QVBoxLayout(self)
        layout.addLayout(bar)
        layout.addWidget(self.canvas, 1)
        layout.addWidget(self.stats)

        self.timer = QTimer(self)
        self.timer.setInterval(self.blink_ms.value())
        self.timer.timeout.connect(self._blink)

    def set_captures(self, before_path, after_path, label=''):
        self.before_path, self.after_path, self.label = before_path, after_path, label
        self.before = imagediff.load(before_path) if before_path and Path(before_path).exists() else None
        self.after = imagediff.load(after_path) if after_path and Path(after_path).exists() else None
        self.refresh()

    def open_images(self):
        first, _ = QFileDialog.getOpenFileName(self, 'BEFORE image', '', 'Images (*.png *.ppm *.bmp *.jpg)')
        if not first:
            return
        second, _ = QFileDialog.getOpenFileName(self, 'AFTER image', str(Path(first).parent),
                                                'Images (*.png *.ppm *.bmp *.jpg)')
        if second:
            self.set_captures(first, second, 'opened by hand')

    def save_view(self):
        image = self.canvas.image
        if image is None:
            return
        path, _ = QFileDialog.getSaveFileName(self, 'Save the current view', 'comparison.png', 'PNG (*.png)')
        if path:
            image.save(path)

    def _pair(self):
        if self.before is None or self.after is None:
            return None, None
        return imagediff.pair(self.before, self.after, self.fit.isChecked())

    def _blink(self):
        self.blink_state = not self.blink_state
        self.refresh(from_timer=True)

    def _toggle_blink_manually(self):
        if MODES[self.mode.currentIndex()] == 'Blink':
            self._blink()

    def refresh(self, *_args, from_timer=False):
        mode = MODES[self.mode.currentIndex()]
        if mode == 'Blink' and not self.timer.isActive():
            self.timer.start()
        elif mode != 'Blink' and self.timer.isActive():
            self.timer.stop()
        self.opacity_label.setText(f'AFTER {self.opacity.value()}%')
        if self.before is None and self.after is None:
            self.canvas.set_image(None)
            self.stats.setText('')
            return
        if self.before is None or self.after is None:
            only = self.before if self.before is not None else self.after
            which = 'BEFORE' if self.before is not None else 'AFTER'
            self.canvas.set_image(to_qimage(only), f'{which} only ({self.label}) - the other capture is missing')
            self.stats.setText('')
            return
        a, b = self._pair()
        diff_image, stats = imagediff.difference(a, b, self.threshold.value(), self.amplify.value())
        caption = self.label
        if mode == 'Side by side':
            image, caption = imagediff.side_by_side(a, b), f'BEFORE | AFTER   {self.label}'
        elif mode == 'Overlay':
            image = imagediff.blend(a, b, self.opacity.value() / 100.0)
            caption = f'overlay: BEFORE {100 - self.opacity.value()}% + AFTER {self.opacity.value()}%   {self.label}'
        elif mode == 'Blink':
            image = b if self.blink_state else a
            caption = ('AFTER' if self.blink_state else 'BEFORE') + f'   (blinking; click to step)   {self.label}'
        elif mode == 'Difference':
            image = diff_image
            caption = f'|BEFORE - AFTER| x{self.amplify.value()}, red over {self.threshold.value()}   {self.label}'
        elif mode == 'BEFORE only':
            image, caption = a, f'BEFORE   {self.label}'
        else:
            image, caption = b, f'AFTER   {self.label}'
        self.canvas.set_image(to_qimage(image), caption)
        if not from_timer:
            sizes = f'BEFORE {self.before.shape[1]}x{self.before.shape[0]}, AFTER {self.after.shape[1]}x' \
                    f'{self.after.shape[0]}'
            self.stats.setText(f'<b>{imagediff.describe(stats)}</b><br>{sizes}; files: '
                               f'{Path(self.before_path).name} | {Path(self.after_path).name}')
            self.stats.setToolTip(f'{self.before_path}\n{self.after_path}')
