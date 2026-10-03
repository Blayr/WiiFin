#!/usr/bin/env bash
# Persistent powerpc-eabi-gdb session attached to Dolphin's GDB stub.
# Start Dolphin with GDB=1 tools/dev/run-dolphin.sh first.
#
# Dolphin's stub waits for one debugger connection before emulation starts,
# and stops listening once that connection closes -- so this keeps ONE gdb
# session alive for the whole run instead of connecting per query.
#
#   tools/dev/dolphin-gdb.sh start            connect and let emulation run
#   tools/dev/dolphin-gdb.sh stop-here        interrupt (pause the emulated CPU)
#   tools/dev/dolphin-gdb.sh cmd "bt 20" ...  run gdb commands (CPU must be paused),
#                                             prints their output
#   tools/dev/dolphin-gdb.sh cont             resume emulation
#   tools/dev/dolphin-gdb.sh end              detach and remove the gdb container
#
# gdb runs in the wiifin-dev image (it has the PowerPC gdb); with Docker
# Desktop the host is reached as host.docker.internal. Symbols come from
# ./WiiFin.elf (override with ELF=path at start).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
NAME=wiifin-gdb
CMDS=build/gdb.cmds
LOG=build/gdb.log
HOST="${GDB_HOST:-host.docker.internal}"
PORT="${GDB_PORT:-2345}"

send() { printf '%s\n' "$@" >> "$CMDS"; }

# Wait for gdb to print a marker after the given commands, then show output.
run_and_show() {
    local mark="__done_$RANDOM$RANDOM"
    local before; before=$(wc -l < "$LOG")
    send "$@" "echo $mark\\n"
    for _ in $(seq 1 60); do
        grep -q "$mark" "$LOG" && break
        sleep 0.5
    done
    tail -n +"$((before + 1))" "$LOG" | grep -v "$mark"
}

case "${1:-}" in
start)
    mkdir -p build; : > "$CMDS"; : > "$LOG"
    docker rm -f "$NAME" >/dev/null 2>&1
    docker run -d --name "$NAME" -v "$PWD:/project" -w /project wiifin-dev \
        sh -c "tail -n +1 -f /project/$CMDS | /opt/devkitpro/devkitPPC/bin/powerpc-eabi-gdb -q -nx /project/${ELF:-WiiFin.elf} > /project/$LOG 2>&1" >/dev/null
    send "set pagination off" "set confirm off" "set height 0" \
         "target remote $HOST:$PORT"
    run_and_show "info registers pc"
    send "continue"
    echo "(emulation continuing)"
    ;;
stop-here)
    docker exec "$NAME" pkill -INT -f powerpc-eabi-gdb
    sleep 1
    run_and_show "info registers pc lr r1"
    ;;
cmd)
    shift; run_and_show "$@"
    ;;
cont)
    send "continue"; echo "(emulation continuing)"
    ;;
end)
    docker exec "$NAME" pkill -INT -f powerpc-eabi-gdb 2>/dev/null; sleep 1
    send "detach" "quit"; sleep 1
    docker rm -f "$NAME" >/dev/null 2>&1; echo "(gdb session ended)"
    ;;
*)
    sed -n '2,20p' "$0"; exit 1 ;;
esac
