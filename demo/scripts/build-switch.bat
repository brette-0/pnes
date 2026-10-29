@echo off
rem Build the Nintendo Switch demo (devkitA64/libnx).
rem Requires %DEVKITPRO% to be set (devkitA64 + libnx).
rem   demo\scripts\build-switch.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" switch %*
exit /b %errorlevel%
