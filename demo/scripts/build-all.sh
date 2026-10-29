#!/usr/bin/env bash
# Build every target whose preset can configure on this host.
#   demo/scripts/build-all.sh [release|debug] [options]   (default: release)
#
# Presets guard themselves with conditions, so anything unavailable on this
# host is simply skipped: the win-* presets only configure on Windows, the
# mac-* presets only on Darwin, the devkitPro targets (ctr/gc/wii/switch/
# wiiu/nds/dsi) need $DEVKITPRO, and ps2 needs $PS2DEV.
#
# linux-docker is left out (it is the same build as linux, routed through the
# CLion Docker toolchain). PS3, GBA and PSP are not supported by this demo.
set -uo pipefail
HERE="$(dirname "${BASH_SOURCE[0]}")"

TARGETS=(
    nes nes-pal
    linux linux-x86 linux-arm64
    win-x64 win-x86 win-arm64
    mac-arm64 mac-x64
    ctr gc gc-pal wii switch wiiu nds dsi
    ps2
)

ok=() failed=()
for t in "${TARGETS[@]}"; do
    echo "==================== ${t} ===================="
    if "${HERE}/build.sh" "$t" "$@"; then
        ok+=("$t")
    else
        echo "!! ${t} build failed or preset unavailable on this host" >&2
        failed+=("$t")
    fi
done

echo "==================== summary ===================="
echo "built:   ${ok[*]:-(none)}"
echo "skipped/failed: ${failed[*]:-(none)}"
[[ ${#failed[@]} -eq 0 ]]
