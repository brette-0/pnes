@echo off
rem Build the Wii demo (devkitPPC/libogc).
rem Requires %DEVKITPRO% to be set (devkitPPC + libogc).
rem   demo\scripts\build-wii.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" wii %*
exit /b %errorlevel%
