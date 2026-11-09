@echo off
rem Developer build: Build.cmd [-Run [game options]] [-Disc <folder>] [-Target <name>] [-Reconfigure] [-Clean] [-Jobs <n>]
rem See dev\Build.ps1 for details. Players use Install.cmd instead.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev\Build.ps1" %*
exit /b %ERRORLEVEL%
