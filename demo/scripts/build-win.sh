#!/usr/bin/env bash
# Build the Windows x64 desktop demo (SDL3). The win presets only configure
# on a Windows host (e.g. Git Bash / MSYS2); they are a no-op elsewhere.
#   demo/scripts/build-win.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" win-x64 "$@"
