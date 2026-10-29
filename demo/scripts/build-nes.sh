#!/usr/bin/env bash
# Build the NES ROM (demo.nes, NTSC) via llvm-mos-sdk.
# Requires CMAKE_PREFIX_PATH (and possibly CC65_DIR) set in local.cmake.
#   demo/scripts/build-nes.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" nes "$@"
