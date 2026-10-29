#!/usr/bin/env bash
# Build the macOS ARM64 (Apple silicon) desktop demo (SDL3). The mac presets
# only configure on a Darwin host; they are a no-op elsewhere.
#   demo/scripts/build-mac.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" mac-arm64 "$@"
