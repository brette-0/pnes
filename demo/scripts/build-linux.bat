@echo off
rem Build the Linux x64 desktop demo (SDL3). Linux hosts / WSL only.
rem   demo\scripts\build-linux.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" linux %*
exit /b %errorlevel%
