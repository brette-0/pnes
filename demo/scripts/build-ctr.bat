@echo off
rem Build the Nintendo 3DS demo (devkitARM/libctru).
rem Requires %DEVKITPRO% to be set (devkitARM + libctru).
rem   demo\scripts\build-ctr.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" ctr %*
exit /b %errorlevel%
