#!/usr/bin/env bash
# Build the macOS x64 (Intel) desktop demo (SDL3).
# Darwin hosts only.
#   demo/scripts/build-mac-x64.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" mac-x64 "$@"
