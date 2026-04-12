#!/usr/bin/env bash
set -euo pipefail

# Set devkitPro defaults if not already in the environment
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=${DEVKITPRO}/devkitPPC}"
export DEVKITPRO DEVKITPPC

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

exec make -j"${JOBS}" "$@"
