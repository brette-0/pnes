@echo off
rem Build the Linux ARM64 desktop demo (SDL3). Linux hosts / WSL only.
rem   demo\scripts\build-linux-arm64.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" linux-arm64 %*
exit /b %errorlevel%
