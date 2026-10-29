#!/usr/bin/env bash
# Build the Linux x64 desktop demo (SDL3).
#   demo/scripts/build-linux.sh [release|debug] [options]   (default: release)
# To package an AppImage instead (needs linuxdeploy on PATH):
#   demo/scripts/build-linux.sh release --target demo-appimage
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" linux "$@"
