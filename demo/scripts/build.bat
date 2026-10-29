@echo off
rem
rem Workhorse build script. Configures (via CMake preset) and builds one target.
rem
rem Usage:
rem   demo\scripts\build.bat <target> [release|debug] [options]
rem
rem   <target>        nes | nes-pal |
rem                   linux | linux-x86 | linux-arm64 | linux-docker |
rem                   win | win-x86 | win-arm64 |
rem                   mac | mac-x64 |
rem                   ctr | gc | gc-pal | wii | switch | wiiu | nds | dsi | ps2
rem   [release|debug] build type (default: release)
rem
rem   'win' is an alias for win-x64, 'mac' for mac-arm64, 'linux' is x64.
rem   PS3, GBA and PSP are not supported by this demo.
rem
rem Options:
rem   -c, --clean     remove the build directory before configuring
rem   -t, --target T  build only the named CMake target (e.g. demo)
rem   -j, --jobs N    parallel build jobs (default: all cores)
rem   -h, --help      show this help
rem
rem The thin wrappers (build-nes.bat, build-linux.bat, ...) just call this.

setlocal EnableDelayedExpansion

rem Repo root is two levels up now that the scripts live under demo\scripts\.
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"

set "TARGETS= nes nes-pal linux linux-x86 linux-arm64 linux-docker win-x64 win-x86 win-arm64 mac-arm64 mac-x64 ctr gc gc-pal wii switch wiiu nds dsi ps2 "

set "PLATFORM="
set "BUILD_TYPE=release"
set "CLEAN=0"
set "CMAKE_TARGET="
set "JOBS="

:parse
if "%~1"=="" goto parsed
set "ARG=%~1"
if /i "!ARG!"=="release"   ( set "BUILD_TYPE=release" & shift & goto parse )
if /i "!ARG!"=="debug"     ( set "BUILD_TYPE=debug"   & shift & goto parse )
if /i "!ARG!"=="-c"        ( set "CLEAN=1" & shift & goto parse )
if /i "!ARG!"=="--clean"   ( set "CLEAN=1" & shift & goto parse )
if /i "!ARG!"=="-t"        ( set "CMAKE_TARGET=%~2" & shift & shift & goto parse )
if /i "!ARG!"=="--target"  ( set "CMAKE_TARGET=%~2" & shift & shift & goto parse )
if /i "!ARG!"=="-j"        ( set "JOBS=%~2" & shift & shift & goto parse )
if /i "!ARG!"=="--jobs"    ( set "JOBS=%~2" & shift & shift & goto parse )
if /i "!ARG!"=="-h"        ( goto usage )
if /i "!ARG!"=="--help"    ( goto usage )
if /i "!ARG!"=="win"       ( set "PLATFORM=win-x64"   & shift & goto parse )
if /i "!ARG!"=="mac"       ( set "PLATFORM=mac-arm64" & shift & goto parse )
echo !TARGETS! | findstr /i /c:" !ARG! " >nul
if errorlevel 1 (
    echo error: unknown argument '!ARG!' 1>&2
    goto usage_err
)
set "PLATFORM=!ARG!"
shift
goto parse

:parsed
if "!PLATFORM!"=="" (
    echo error: no target given ^(one of:!TARGETS!^) 1>&2
    goto usage_err
)

set "PRESET=!PLATFORM!-!BUILD_TYPE!"
set "BUILD_DIR=!ROOT!\builds\cmake-build-!PRESET!"

if "!CLEAN!"=="1" if exist "!BUILD_DIR!" (
    echo ^>^> cleaning !BUILD_DIR!
    rmdir /s /q "!BUILD_DIR!"
)

echo ^>^> configuring preset '!PRESET!'
cmake --preset "!PRESET!" -S "!ROOT!" || exit /b 1

set "BUILD_ARGS=--build "!BUILD_DIR!""
if not "!CMAKE_TARGET!"=="" set "BUILD_ARGS=!BUILD_ARGS! --target !CMAKE_TARGET!"
if "!JOBS!"=="" (
    set "BUILD_ARGS=!BUILD_ARGS! --parallel"
) else (
    set "BUILD_ARGS=!BUILD_ARGS! --parallel !JOBS!"
)

echo ^>^> building preset '!PRESET!'
cmake !BUILD_ARGS! || exit /b 1

echo ^>^> done -^> !BUILD_DIR!
exit /b 0

:usage
call :print_usage
exit /b 0

:usage_err
call :print_usage 1>&2
exit /b 2

:print_usage
for /f "tokens=1,* delims=:" %%a in ('findstr /n "^rem" "%~f0"') do (
    if %%a leq 24 (
        set "LINE=%%b"
        set "LINE=!LINE:rem =!"
        if "!LINE!"=="rem" set "LINE="
        echo(!LINE!
    )
)
goto :eof
