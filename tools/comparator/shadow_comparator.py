#!/usr/bin/env python3
"""Shadow Native Rewrite - Before / After Comparator.

A desktop workstation that launches the OLD (legacy) and NEW (native rewrite) builds of the game at the same named
checkpoint - each from its own savestate - drives both through their development link (pause, run N frames, capture,
reload), and compares the captures and the game state side by side. docs/comparator/README.md.

    python tools/comparator/shadow_comparator.py           (or double-click Comparator.bat)

Needs Python 3.10+ with PySide6, Pillow and numpy.
"""
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def main() -> int:
    from PySide6.QtWidgets import QApplication, QMessageBox

    qt = QApplication(sys.argv)
    qt.setApplicationName('Shadow Before/After Comparator')
    log = HERE / 'comparator-errors.log'

    def report(kind, value, tb):
        text = ''.join(traceback.format_exception(kind, value, tb))
        with log.open('a', encoding='utf-8') as f:
            f.write(text + '\n')
        try:
            QMessageBox.critical(None, 'Comparator error', text[-3000:])
        except Exception:  # noqa: BLE001
            pass

    sys.excepthook = report
    from comparator.app import Comparator
    from comparator.ui import MainWindow

    app = Comparator()
    window = MainWindow(app)
    window.show()
    return qt.exec()


if __name__ == '__main__':
    sys.exit(main())
