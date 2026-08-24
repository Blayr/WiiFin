#!/usr/bin/env bash
# Boots WiiFin.dol in the Dolphin emulator.
#
# NOTE: /usr/bin/dolphin on this machine is the KDE file manager, not the
# emulator -- the emulator is installed as the org.DolphinEmu.dolphin-emu
# Flatpak, which is what this script launches.
#
# Usage: tools/dev/run-dolphin.sh [-r|--rebuild]
#   tools/dev/run-dolphin.sh              # boot the existing WiiFin.dol
#   tools/dev/run-dolphin.sh --rebuild    # rebuild via docker-build.sh first, then boot
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOL="${PROJECT_DIR}/WiiFin.dol"
DOLPHIN_APP_ID="org.DolphinEmu.dolphin-emu"

rebuild=false
for arg in "$@"; do
    case "$arg" in
        -r|--rebuild) rebuild=true ;;
        *)
            echo "Usage: $0 [-r|--rebuild]" >&2
            exit 1
            ;;
    esac
done

if [ "$rebuild" = true ]; then
    "${PROJECT_DIR}/tools/dev/docker-build.sh"
fi

if [ ! -f "$DOL" ]; then
    echo "error: ${DOL} not found -- build it first (tools/dev/build.sh or pass --rebuild)" >&2
    exit 1
fi

if ! flatpak info "${DOLPHIN_APP_ID}" >/dev/null 2>&1; then
    echo "error: Dolphin Emulator Flatpak (${DOLPHIN_APP_ID}) is not installed" >&2
    exit 1
fi

echo "==> Booting ${DOL} in Dolphin"
exec flatpak run "${DOLPHIN_APP_ID}" -e "${DOL}"
