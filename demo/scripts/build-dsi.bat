@echo off
rem Build the Nintendo DSi demo (devkitARM/libnds).
rem Requires %DEVKITPRO% to be set (devkitARM + libnds).
rem   demo\scripts\build-dsi.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" dsi %*
exit /b %errorlevel%
