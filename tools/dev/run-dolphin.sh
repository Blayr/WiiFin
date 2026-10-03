#!/usr/bin/env bash
# Boot a WiiFin build in the Dolphin emulator with SYS_Report/OSReport
# logging, then save that run's logs under build/.
#
# NOTE: /usr/bin/dolphin on this machine is the KDE file manager, not the
# emulator -- the emulator is installed as the org.DolphinEmu.dolphin-emu
# Flatpak, which is what this script launches.
#
#   tools/dev/run-dolphin.sh                 boots ./WiiFin.dol
#   tools/dev/run-dolphin.sh -r              rebuilds via docker-build.sh first, then boots
#   tools/dev/run-dolphin.sh path/to.dol     boots another build
#   SECS=60 tools/dev/run-dolphin.sh         auto-stop after N seconds
#                                            (default: run until the window is closed)
#   GCPAD=0 tools/dev/run-dolphin.sh         don't set up the GameCube controller
#   DEBUG=1 tools/dev/run-dolphin.sh         open Dolphin's debugger UI (code view,
#                                            breakpoints, memory breakpoints -- hits
#                                            with "log" are written to the log via the
#                                            MI category, writer PC + symbol included)
#   GDB=1 tools/dev/run-dolphin.sh           enable Dolphin's GDB stub on localhost:2345 --
#                                            attach with tools/dev/dolphin-gdb.sh
#                                            (registers, memory, backtraces w/ symbols)
#   STRICT=1 tools/dev/run-dolphin.sh        MMU emulation: invalid memory accesses crash
#                                            like on a real Wii (slower)
#   WIIMOTE=1 tools/dev/run-dolphin.sh       keep the Wiimotes connected too
#                                            (by default all four are disconnected while
#                                            Dolphin runs -- see the controller notes)
#
# Controller: by default GameCube port 1 is a standard controller mapped from
# Dolphin's saved GCPad profile "8bitdo" (Profiles/GCPad/8bitdo.ini; override
# with GCPAD_PROFILE=name). The profile is copied into GCPadNew.ini's
# [GCPad1] section before launch -- the one persistent config edit this
# script makes, so the mapping is right even if it was changed in the GUI.
# WiiFin accepts the GC pad everywhere (menus and the in-video overlay).
# All four Wiimotes are disconnected for the run (WiimoteNew.ini is edited,
# then restored on exit): the emulated Wiimote's IR pointer follows the
# mouse, and WiiFin's menus snap the selection to wherever it hovers, which
# makes the D-pad/stick look broken.
#
# Outputs:
#   build/dolphin-last.log    Dolphin's log for the run -- every SYS_Report
#                             line WiiFin and MPlayer print (invisible on real
#                             hardware without a USB Gecko)
#   build/dolphin-wiifin.log  WiiFin's own SD-card debug log (DebugLog), if
#                             "Debug Log File" is on in WiiFin's settings
#
# All settings below are passed with -C, so they apply to this run only --
# Dolphin's saved config is never modified.
#
# ---------------------------------------------------------------------------
# Requirements (each one was a hard failure until fixed):
#
# 1. Dolphin's data folder Load/ must be WRITABLE. Dolphin keeps the virtual
#    Wii SD card image (Load/WiiSD.raw) there; if it can't create it, any
#    homebrew that initializes storage (fatInitDefault / dvmInitDefault,
#    which GRRLIB_Init calls) crashes Dolphin itself with a segfault. Checked
#    below.
#
# 2. The DOL's sections must be 32-byte aligned in size. Newer Dolphin
#    refuses anything else ("Failed to init core" in the GUI, "Could not boot
#    the specified file" on the command line). tools/wii_wiifin.ld pads each
#    loadable section for this; HBC doesn't care either way.
#
# Video playback additionally needs (set below):
#
# 3. DSPHLE=False (LLE DSP emulation). MPlayer's audio output uses libaesnd,
#    whose custom DSP microcode Dolphin's default HLE audio doesn't run.
#    (Menu audio uses ASND, which HLE does support.)
#
# 4. CPUThread=True + SyncGPU=True + GFX DeferEFBCopies=False. Without these
#    Dolphin never raises the GX draw-done (PE finish) interrupt for
#    MPlayer's frames, so MPlayer blocks in GX_WaitDrawDone() after the first
#    frame: black screen, no audio. (Not yet narrowed down which of the three
#    is the one that matters.)
#
# Other run settings:
#   ConfirmStop=False           no "stop emulation?" prompt when this script
#                               or a timeout closes Dolphin
#   WiiSDCardEnableFolderSync   the virtual SD card is built from Dolphin's
#                               Load/WiiSDSync folder at boot and written back
#                               on stop. Put apps/WiiFin/wiifin.cfg there
#                               (copy it off a real Wii's SD card) to start
#                               with a saved profile instead of the connect
#                               screen. That file contains an access token --
#                               keep it there, not in this repo.
# ---------------------------------------------------------------------------
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

APP=org.DolphinEmu.dolphin-emu
DATA="$HOME/.var/app/$APP/data/dolphin-emu"

rebuild=false
DOL_ARG="WiiFin.dol"
for arg in "$@"; do
    case "$arg" in
        -r|--rebuild) rebuild=true ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *)            DOL_ARG="$arg" ;;
    esac
done
if [[ "$rebuild" == true ]]; then
    tools/dev/docker-build.sh || exit 1
fi
if [[ ! -f "$DOL_ARG" ]]; then
    echo "error: $DOL_ARG not found -- build it first (tools/dev/docker-build.sh, or pass -r)" >&2
    exit 1
fi
if ! flatpak info "$APP" >/dev/null 2>&1; then
    echo "error: Dolphin Emulator Flatpak ($APP) is not installed" >&2
    exit 1
fi
DOL="$(realpath "$DOL_ARG")"
LOG="$DATA/Logs/dolphin.log"
SYNC="$DATA/Load/WiiSDSync/apps/WiiFin"
mkdir -p build

# Requirement 1.
if [[ -d "$DATA/Load" && ! -w "$DATA/Load" ]]; then
    echo "ERROR: $DATA/Load is not writable -- Dolphin can't create its virtual SD" >&2
    echo "card and will crash. Fix with:  chmod u+w \"$DATA/Load\"" >&2
    exit 1
fi
[[ -f "$SYNC/wiifin.cfg" ]] || echo "Note: no $SYNC/wiifin.cfg -- WiiFin will start at the connect screen."

flatpak kill "$APP" 2>/dev/null && sleep 1   # stop any running Dolphin first

# STRICT=1: full MMU emulation. Dolphin then raises real DSI exceptions on
# invalid memory accesses like the Wii's CPU does (WiiFin's crash screen
# appears) instead of tolerating them. Slower -- playback may stutter.
STRICT_ARGS=()
[[ "${STRICT:-0}" == 1 ]] && STRICT_ARGS=(-C Dolphin.Core.MMU=True) && echo "STRICT: MMU emulation on"
# DEBUG=1: debugger UI. Memory-breakpoint hits are logged at Notice level
# under MI (already enabled below), so they land in build/dolphin-last.log.
[[ "${DEBUG:-0}" == 1 ]] && STRICT_ARGS+=(-d) && echo "DEBUG: debugger UI on"
# GDB=1: Dolphin's built-in GDB remote stub. Emulation may wait for the first
# debugger connection before starting -- tools/dev/dolphin-gdb.sh handles it.
[[ "${GDB:-0}" == 1 ]] && STRICT_ARGS+=(-C Dolphin.General.GDBPort=2345) && echo "GDB: stub on localhost:2345"

PAD_ARGS=()
if [[ "${GCPAD:-1}" != 0 ]]; then
    CFG="$HOME/.var/app/$APP/config/dolphin-emu"
    PROFILE="$CFG/Profiles/GCPad/${GCPAD_PROFILE:-8bitdo}.ini"
    if [[ -f "$PROFILE" ]]; then
        # Replace (or add) [GCPad1] with the profile's mapping.
        python3 - "$CFG/GCPadNew.ini" "$PROFILE" <<'PYEOF'
import sys, re, os
pad_path, prof_path = sys.argv[1], sys.argv[2]
body = open(prof_path).read().split("[Profile]\n", 1)[1]
section = "[GCPad1]\n" + body.rstrip("\n") + "\n"
pad = open(pad_path).read() if os.path.exists(pad_path) else ""
if "[GCPad1]" in pad:
    pad = re.sub(r"\[GCPad1\].*?(?=\n\[|\Z)", section.rstrip("\n"), pad, count=1, flags=re.DOTALL)
else:
    pad = section + pad
open(pad_path, "w").write(pad)
PYEOF
        echo "GameCube port 1: $(grep -m1 '^Device' "$PROFILE" | cut -d= -f2-) (profile ${GCPAD_PROFILE:-8bitdo})"
    else
        echo "Note: GCPad profile $PROFILE not found -- port 1 keeps its current mapping."
    fi
    # 6 = Standard Controller. BackgroundInput keeps the pad working when the
    # render window isn't the focused window.
    PAD_ARGS=(-C Dolphin.Core.SIDevice0=6 -C Dolphin.Input.BackgroundInput=True)
    # Dolphin ignores -C for Wiimote sources, so disconnect all four in
    # WiimoteNew.ini for the run and put the user's file back on exit (the
    # EXIT trap also fires if Dolphin is killed or this script is interrupted).
    WIIMOTE_INI="$CFG/WiimoteNew.ini"
    if [[ "${WIIMOTE:-0}" != 1 && -f "$WIIMOTE_INI" ]]; then
        cp "$WIIMOTE_INI" build/WiimoteNew.ini.user
        trap 'cp build/WiimoteNew.ini.user "$WIIMOTE_INI" 2>/dev/null' EXIT
        python3 - "$WIIMOTE_INI" <<'PYEOF'
import sys, re
p = sys.argv[1]
s = open(p).read()
for n in range(1, 5):
    sec = f"[Wiimote{n}]"
    if sec not in s:
        s += f"\n{sec}\nSource = 0\n"
        continue
    m = re.search(rf"{re.escape(sec)}\n(.*?)(?=\n\[|\Z)", s, re.DOTALL)
    body = m.group(1)
    if re.search(r"(?m)^Source\s*=", body):
        body2 = re.sub(r"(?m)^Source\s*=.*$", "Source = 0", body)
    else:
        body2 = "Source = 0\n" + body
    s = s[:m.start(1)] + body2 + s[m.end(1):]
open(p, "w").write(s)
PYEOF
        echo "Wiimotes: all disconnected for this run (WiimoteNew.ini restored on exit)"
    fi
fi
: > "$LOG" 2>/dev/null || true                 # start this run with an empty log

# -b (batch) hides Dolphin's main window -- and with it the debugger -- so
# it's left out in DEBUG mode (Dolphin then stays open until closed).
BATCH=(-b)
[[ "${DEBUG:-0}" == 1 ]] && BATCH=()

ARGS=("${BATCH[@]}"
      -C Dolphin.Interface.ConfirmStop=False
      -C Dolphin.Core.WiiSDCardEnableFolderSync=True
      # Requirement 3 (playback audio):
      -C Dolphin.Core.DSPHLE=False
      # Requirement 4 (playback video):
      -C Dolphin.Core.CPUThread=True
      -C Dolphin.Core.SyncGPU=True
      -C GFX.Hacks.DeferEFBCopies=False
      # Logging -- OSREPORT carries SYS_Report output (logged at Notice
      # level). Verbosity 3 = Notice/Error/Warning only, so the categories
      # below add only problems, not chatter: invalid memory accesses (MI),
      # CPU exceptions (PowerPC), and anything the GPU emulation chokes on
      # (CP/PE/GP/Video) -- the things that crash or hang a real Wii but that
      # Dolphin otherwise tolerates silently.
      -C Logger.Options.WriteToFile=True
      -C Logger.Options.Verbosity=3
      -C Logger.Logs.OSREPORT=True
      -C Logger.Logs.OSREPORT_HLE=True
      -C Logger.Logs.MI=True
      -C Logger.Logs.PowerPC=True
      -C Logger.Logs.CP=True
      -C Logger.Logs.PE=True
      -C Logger.Logs.GP=True
      -C Logger.Logs.Video=True
      -C Logger.Logs.PI=True
      "${PAD_ARGS[@]}"
      "${STRICT_ARGS[@]}"
      -e "$DOL")

echo "Booting $DOL in Dolphin..."
if [[ -n "${SECS:-}" ]]; then
    timeout "$SECS" flatpak run "$APP" "${ARGS[@]}" >/dev/null 2>&1
else
    flatpak run "$APP" "${ARGS[@]}" >/dev/null 2>&1
fi

cp "$LOG" build/dolphin-last.log 2>/dev/null
echo "Dolphin exited. $(grep -c OSREPORT build/dolphin-last.log 2>/dev/null || echo 0) report lines saved to build/dolphin-last.log"
# 0d8005a0 = GRRLIB_Init's Wii U detection read (Hollywood chip-revision
# register) -- expected on every boot, not a problem.
W=$(grep -v OSREPORT build/dolphin-last.log 2>/dev/null | grep -E " [EW]\[" | grep -vc "addr=0d8005a0")
[[ "$W" -gt 0 ]] && echo "Dolphin logged $W emulation errors/warnings -- see build/dolphin-last.log"
if [[ -f "$SYNC/wiifin_log_idx" ]] && cp "$SYNC/wiifin_log_$(cat "$SYNC/wiifin_log_idx").txt" build/dolphin-wiifin.log 2>/dev/null; then
    echo "WiiFin's own debug log saved to build/dolphin-wiifin.log"
fi
