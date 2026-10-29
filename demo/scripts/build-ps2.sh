#!/usr/bin/env bash
# Build the PlayStation 2 demo (ps2dev: ps2sdk + gsKit).
# Requires $PS2DEV (and PS2SDK/GSKIT) to be set -- see docker/Dockerfile.ps2
# and the ps2-* services in compose.yml for a ready-made container.
#   demo/scripts/build-ps2.sh [release|debug] [options]   (default: release)
set -euo pipefail
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh" ps2 "$@"
