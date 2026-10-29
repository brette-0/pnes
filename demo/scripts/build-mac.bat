@echo off
rem Build the macOS ARM64 (Apple silicon) desktop demo (SDL3).
rem The mac-* presets only configure on a Darwin host.
rem   demo\scripts\build-mac.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" mac-arm64 %*
exit /b %errorlevel%
