#!/usr/bin/env bash
# Build the Linux x86 (32-bit) desktop demo (SDL3).
# Requires 32-bit multilib toolchain and dev libraries.
#   demo/scripts/build-linux-x86.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" linux-x86 "$@"
