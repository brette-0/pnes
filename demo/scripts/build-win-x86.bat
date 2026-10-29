@echo off
rem Build the Windows x86 (32-bit) desktop demo (SDL3).
rem Windows hosts only.
rem   demo\scripts\build-win-x86.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" win-x86 %*
exit /b %errorlevel%
