@echo off
rem Build the GameCube demo (devkitPPC/libogc, NTSC 60Hz).
rem Requires %DEVKITPRO% to be set (devkitPPC + libogc).
rem   demo\scripts\build-gc.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" gc %*
exit /b %errorlevel%
