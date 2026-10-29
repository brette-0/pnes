@echo off
rem Build the PlayStation 2 demo (ps2dev: ps2sdk + gsKit).
rem Requires %PS2DEV% (and PS2SDK/GSKIT) to be set -- see docker\Dockerfile.ps2
rem and the ps2-* services in compose.yml for a ready-made container.
rem   demo\scripts\build-ps2.bat [release|debug] [options]   (default: release)
call "%~dp0build.bat" ps2 %*
exit /b %errorlevel%
