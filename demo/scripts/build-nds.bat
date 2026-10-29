@echo off
rem Build the Nintendo DS demo (devkitARM/libnds).
rem Requires %DEVKITPRO% to be set (devkitARM + libnds).
rem   demo\scripts\build-nds.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" nds %*
exit /b %errorlevel%
