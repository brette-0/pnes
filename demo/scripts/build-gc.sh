#!/usr/bin/env bash
# Build the GameCube demo (devkitPPC/libogc, NTSC 60Hz).
# Requires $DEVKITPRO to be set (devkitPPC + libogc).
#   demo/scripts/build-gc.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" gc "$@"
