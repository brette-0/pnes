#!/usr/bin/env bash
# Build the Windows ARM64 desktop demo (SDL3).
# Windows hosts only.
#   demo/scripts/build-win-arm64.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" win-arm64 "$@"
