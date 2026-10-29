@echo off
rem Build the GameCube demo (devkitPPC/libogc, native PAL 50Hz).
rem Requires %DEVKITPRO% to be set (devkitPPC + libogc).
rem   demo\scripts\build-gc-pal.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" gc-pal %*
exit /b %errorlevel%
