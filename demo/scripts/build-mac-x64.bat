@echo off
rem Build the macOS x64 (Intel) desktop demo (SDL3).
rem Darwin hosts only.
rem   demo\scripts\build-mac-x64.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" mac-x64 %*
exit /b %errorlevel%
