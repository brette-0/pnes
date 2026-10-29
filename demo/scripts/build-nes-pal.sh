#!/usr/bin/env bash
# Build the NES ROM (demo.nes, PAL timing) via llvm-mos-sdk.
# Requires CMAKE_PREFIX_PATH (and possibly CC65_DIR) set in local.cmake.
#   demo/scripts/build-nes-pal.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" nes-pal "$@"
