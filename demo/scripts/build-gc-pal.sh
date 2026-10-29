#!/usr/bin/env bash
# Build the GameCube demo (devkitPPC/libogc, native PAL 50Hz).
# Requires $DEVKITPRO to be set (devkitPPC + libogc).
#   demo/scripts/build-gc-pal.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" gc-pal "$@"
