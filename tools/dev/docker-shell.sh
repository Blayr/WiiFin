#!/usr/bin/env bash
# Interactive shell inside the wiifin-dev container, repo mounted at /project.
# Useful for debugging build failures, inspecting the toolchain, or running
# individual make/gcc commands by hand.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="wiifin-dev"

if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    echo "==> Building ${IMAGE} image (one-time, a few minutes)"
    docker build -f "${PROJECT_DIR}/Dockerfile.dev" -t "${IMAGE}" "${PROJECT_DIR}"
fi

docker run --rm -it -v "${PROJECT_DIR}:/project" -w /project \
    -e DEVKITPRO=/opt/devkitpro -e DEVKITPPC=/opt/devkitpro/devkitPPC \
    "${IMAGE}" bash
