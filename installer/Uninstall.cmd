@echo off
setlocal
rem Removes this install of Shadow the Hedgehog Recompiled. Saves and settings are kept unless -RemoveSaves is given.
set "HERE=%~dp0"
cd /d "%TEMP%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%HERE%Uninstall.ps1" -InstallPath "%HERE%." -Launcher %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    if not "%RC%"=="2" pause
    exit /b %RC%
)
rem This file last, then the folder if nothing is left in it.
(goto) 2>nul & del "%~f0" & rmdir "%~dp0." 2>nul
