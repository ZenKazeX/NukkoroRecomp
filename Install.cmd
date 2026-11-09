@echo off
setlocal
rem Shadow the Hedgehog Recompiled setup.
rem   Install.cmd                                   opens the setup window (built here on first use)
rem   Install.cmd -DiscPath <dir> -InstallPath <dir> installs from the console instead
cd /d "%~dp0"
if not "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Install.ps1" %*
    exit /b %ERRORLEVEL%
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found. Install CMake 3.25+ and Visual Studio 2026 with "Desktop development with C++",
    echo then run Install.cmd again. See README.md.
    pause
    exit /b 1
)
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
echo   Install.cmd -DiscPath "D:\Dumps\Shadow PAL" -InstallPath "%USERPROFILE%\Games\Shadow the Hedgehog Recompiled"
pause
exit /b 1