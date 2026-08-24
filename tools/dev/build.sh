#!/usr/bin/env bash
# Builds WiiFin.dol. Thin wrapper around docker-build.sh, kept as a plain
# "build" entry point alongside run-dolphin.sh.
#
# Usage: tools/dev/build.sh [make args...]
#   tools/dev/build.sh              # normal build
#   tools/dev/build.sh clean        # make clean
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "${PROJECT_DIR}/tools/dev/docker-build.sh" "$@"
