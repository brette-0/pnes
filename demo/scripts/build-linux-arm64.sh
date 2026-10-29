#!/usr/bin/env bash
# Build the Linux ARM64 desktop demo (SDL3).
# Requires an aarch64 cross toolchain (or an ARM64 host).
#   demo/scripts/build-linux-arm64.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" linux-arm64 "$@"
