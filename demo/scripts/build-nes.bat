@echo off
rem Build the NES ROM (demo.nes, NTSC) via llvm-mos-sdk.
rem Requires CMAKE_PREFIX_PATH (and possibly CC65_DIR) set in local.cmake.
rem   demo\scripts\build-nes.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" nes %*
exit /b %errorlevel%
