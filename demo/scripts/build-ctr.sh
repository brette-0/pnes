#!/usr/bin/env bash
# Build the Nintendo 3DS demo (devkitARM/libctru).
# Requires $DEVKITPRO to be set (devkitARM + libctru).
#   demo/scripts/build-ctr.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" ctr "$@"
