@echo off
rem Build the Wii U demo (devkitPPC/wut).
rem Requires %DEVKITPRO% to be set (devkitPPC + wut).
rem   demo\scripts\build-wiiu.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" wiiu %*
exit /b %errorlevel%
