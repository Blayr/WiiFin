#!/usr/bin/env bash
# Builds WiiFin.dol inside the wiifin-dev container.
# Builds the wiifin-dev image first if it doesn't exist yet.
# Builds libs/mbedtls/lib (once) if it's missing.
#
# Usage: tools/dev/docker-build.sh [make args...]
#   tools/dev/docker-build.sh              # normal build
#   tools/dev/docker-build.sh clean        # make clean
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="wiifin-dev"

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "==> Building ${IMAGE} image (one-time, a few minutes)"
    docker build -f "${PROJECT_DIR}/Dockerfile.dev" -t "${IMAGE}" "${PROJECT_DIR}"
fi

if [ ! -f "${PROJECT_DIR}/libs/mbedtls/lib/libmbedtls.a" ]; then
    echo "==> libs/mbedtls/lib missing, building mbedTLS first"
    docker run --rm -v "${PROJECT_DIR}:/project" -w /project "${IMAGE}" \
        tools/dev/build_mbedtls.sh
fi

docker run --rm -v "${PROJECT_DIR}:/project" -w /project \
    -e DEVKITPRO=/opt/devkitpro -e DEVKITPPC=/opt/devkitpro/devkitPPC \
    "${IMAGE}" make -j"$(nproc)" "$@"
