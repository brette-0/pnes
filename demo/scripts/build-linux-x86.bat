@echo off
rem Build the Linux x86 (32-bit) desktop demo (SDL3). Linux hosts / WSL only.
rem   demo\scripts\build-linux-x86.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" linux-x86 %*
exit /b %errorlevel%
