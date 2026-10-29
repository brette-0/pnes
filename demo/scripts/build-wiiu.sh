#!/usr/bin/env bash
# Build the Wii U demo (devkitPPC/wut).
# Requires $DEVKITPRO to be set (devkitPPC + wut).
#   demo/scripts/build-wiiu.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" wiiu "$@"
