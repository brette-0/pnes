#!/usr/bin/env bash
# Build the Windows x86 (32-bit) desktop demo (SDL3).
# Windows hosts only.
#   demo/scripts/build-win-x86.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" win-x86 "$@"
