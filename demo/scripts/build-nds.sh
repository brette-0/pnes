#!/usr/bin/env bash
# Build the Nintendo DS demo (devkitARM/libnds).
# Requires $DEVKITPRO to be set (devkitARM + libnds).
#   demo/scripts/build-nds.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" nds "$@"
