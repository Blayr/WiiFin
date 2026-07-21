#!/usr/bin/env bash
# Cross-compiles mbedTLS for the Wii and installs the static libs into
# libs/mbedtls/lib, where WiiFin's top-level Makefile expects them
# (-L$(CURDIR)/libs/mbedtls/lib -lmbedtls -lmbedx509 -lmbedcrypto).
#
# Must run inside the wiifin-dev container (needs powerpc-eabi-gcc on PATH).
# Run via tools/dev/docker-build.sh, or manually:
#   docker run --rm -v "$(pwd)":/project -w /project wiifin-dev tools/dev/build_mbedtls.sh
#
# Why this can't just be `apt install libmbedtls-dev` or a devkitPro portlib:
# WiiFin vendors a project-specific libs/mbedtls/include/mbedtls/mbedtls_config.h
# (bare-metal config: no filesystem, no POSIX threads, no built-in net I/O —
# see that file for the full rationale). The library must be compiled against
# that exact config, so a generic prebuilt mbedTLS won't link/behave correctly.
#
# mbedTLS version: the vendored headers under libs/mbedtls/include were
# diffed against upstream tags to confirm they match v3.6.2 (LTS) aside from
# the intentional config customization — see DEV_SETUP.md for how that was
# verified. If libs/mbedtls/include is ever updated from upstream, bump the
# MBEDTLS_REF below to match.
set -euo pipefail

MBEDTLS_REF="v3.6.2"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=${DEVKITPRO}/devkitPPC}"
export PATH="${DEVKITPPC}/bin:${PATH}"

if ! command -v powerpc-eabi-gcc >/dev/null 2>&1; then
    echo "error: powerpc-eabi-gcc not found. Run this inside the wiifin-dev container." >&2
    exit 1
fi

echo "==> Cloning mbedTLS ${MBEDTLS_REF}"
git clone --quiet --depth 1 --branch "${MBEDTLS_REF}" \
    https://github.com/Mbed-TLS/mbedtls.git "${WORK_DIR}/mbedtls"
git -C "${WORK_DIR}/mbedtls" submodule update --init --recursive --depth 1 framework

echo "==> Overlaying WiiFin's project-specific config/headers"
rm -rf "${WORK_DIR}/mbedtls/include"
cp -r "${PROJECT_DIR}/libs/mbedtls/include" "${WORK_DIR}/mbedtls/include"

echo "==> Cross-compiling libmbedcrypto / libmbedx509 / libmbedtls"
make -C "${WORK_DIR}/mbedtls/library" \
    CC=powerpc-eabi-gcc AR=powerpc-eabi-ar \
    CFLAGS="-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -O2 -I${WORK_DIR}/mbedtls/include" \
    static

echo "==> Installing to libs/mbedtls/lib"
mkdir -p "${PROJECT_DIR}/libs/mbedtls/lib"
cp "${WORK_DIR}/mbedtls/library/"libmbedcrypto.a \
   "${WORK_DIR}/mbedtls/library/"libmbedx509.a \
   "${WORK_DIR}/mbedtls/library/"libmbedtls.a \
   "${PROJECT_DIR}/libs/mbedtls/lib/"

echo "==> Done: $(ls "${PROJECT_DIR}/libs/mbedtls/lib")"
