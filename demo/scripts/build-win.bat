@echo off
rem Build the Windows x64 desktop demo (SDL3).
rem The win-* presets only configure on a Windows host.
rem   demo\scripts\build-win.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" win-x64 %*
exit /b %errorlevel%
