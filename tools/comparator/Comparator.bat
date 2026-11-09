@echo off
rem Shadow Native Rewrite - Before / After Comparator (docs\comparator\README.md).
rem Double-click: starts the workstation without a console window. Needs Python with PySide6, Pillow, numpy.
setlocal
set "HERE=%~dp0"
where pythonw >nul 2>nul
if %ERRORLEVEL%==0 (
  start "" pythonw "%HERE%shadow_comparator.py"
) else (
  python "%HERE%shadow_comparator.py"
)
