@echo off
rem Build the Windows ARM64 desktop demo (SDL3).
rem Windows hosts only.
rem   demo\scripts\build-win-arm64.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" win-arm64 %*
exit /b %errorlevel%
