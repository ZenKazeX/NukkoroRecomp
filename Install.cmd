@echo off
setlocal
rem Shadow the Hedgehog setup (the PC recompilation).
rem   Install.cmd                                   opens the setup window (built here on first use)
rem   Install.cmd -DiscPath <dir> -InstallPath <dir> installs from the console instead
cd /d "%~dp0"
if not "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Install.ps1" %*
    exit /b %ERRORLEVEL%
)

rem The CMake the installer itself would use: Visual Studio 2026's own first, then one on PATH that knows Visual Studio
rem 2026 (an older CMake fails with "Could not create named generator"). Its folder goes first on PATH.
set "NUKKORO_CORE=%~dp0installer\InstallCore.psm1"
set "NUKKORO_CMAKE="
for /f "usebackq delims=" %%C in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Import-Module $env:NUKKORO_CORE; (Test-Prerequisites | Where-Object { $_.Name -like 'CMake*' }).Value"`) do set "NUKKORO_CMAKE=%%C"
if not defined NUKKORO_CMAKE (
    echo No CMake that knows Visual Studio 2026 was found. Install Visual Studio 2026 with "Desktop development with C++"
    echo ^(it brings its own CMake^), or CMake 4.2 or newer, then run Install.cmd again. See README.md.
    pause
    exit /b 1
)
for %%D in ("%NUKKORO_CMAKE%") do set "PATH=%%~dpD;%PATH%"
echo Preparing the setup window...
cmake -S installer -B .build\setup -G "Visual Studio 18 2026" -A x64 >.build-setup.log 2>&1
if errorlevel 1 goto failed
cmake --build .build\setup --config Release -- /nodeReuse:false /verbosity:minimal >>.build-setup.log 2>&1
if errorlevel 1 goto failed
del .build-setup.log >nul 2>nul
start "" "%~dp0.build\setup\Release\NukkoroSetup.exe" --installer-dir "%~dp0installer"
exit /b 0

:failed
type .build-setup.log
echo.
echo The setup window could not be built (log above: .build-setup.log).
echo You can still install from the console:
echo   Install.cmd -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "%USERPROFILE%\Games\Shadow the Hedgehog"
pause
exit /b 1