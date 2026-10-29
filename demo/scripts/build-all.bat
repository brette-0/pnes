@echo off
rem Build every target whose preset can configure on this host.
rem   demo\scripts\build-all.bat [release|debug] [options]   (default: release)
rem
rem Presets guard themselves with conditions, so anything unavailable on this
rem host is simply skipped: the win-* presets only configure on Windows, the
rem mac-* presets only on Darwin, the devkitPro targets (ctr/gc/wii/switch/
rem wiiu/nds/dsi) need %DEVKITPRO%, and ps2 needs %PS2DEV%.
rem
rem linux-docker is left out (it is the same build as linux, routed through the
rem CLion Docker toolchain). PS3, GBA and PSP are not supported by this demo.

setlocal EnableDelayedExpansion

set "TARGETS=nes nes-pal linux linux-x86 linux-arm64 win-x64 win-x86 win-arm64 mac-arm64 mac-x64 ctr gc gc-pal wii switch wiiu nds dsi ps2"

set "OK="
set "FAILED="

for %%T in (%TARGETS%) do (
    echo ==================== %%T ====================
    call "%~dp0build.bat" %%T %*
    if errorlevel 1 (
        echo ** %%T build failed or preset unavailable on this host 1>&2
        set "FAILED=!FAILED! %%T"
    ) else (
        set "OK=!OK! %%T"
    )
)

echo ==================== summary ====================
if "!OK!"=="" ( echo built:   ^(none^) ) else ( echo built:  !OK! )
if "!FAILED!"=="" ( echo skipped/failed: ^(none^) ) else ( echo skipped/failed:!FAILED! )

if not "!FAILED!"=="" exit /b 1
exit /b 0
