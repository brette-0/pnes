#!/usr/bin/env bash
# Build the Nintendo Switch demo (devkitA64/libnx).
# Requires $DEVKITPRO to be set (devkitA64 + libnx).
#   demo/scripts/build-switch.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" switch "$@"
