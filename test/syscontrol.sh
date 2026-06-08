#!/bin/bash
# System-control functional test: poweroff / reboot / exit via the wsh supervisor.
#
# Drives the wsh debug supervisor through the engine's stdio console (a FIFO held
# open so the engine never sees a premature EOF) and asserts the three control
# paths, including that the engine's console survives a supervisor teardown:
#
#   1. poweroff -> the engine exits without respawning the supervisor.
#   2. exit     -> the supervisor exits and is respawned WITH a working console:
#                  a command issued after the respawn is processed. This is the
#                  regression guard for the console fds (0/1/2) being borrowed,
#                  not owned, by a wapp's VFS — a teardown must not close them.
#   3. reboot   -> the engine re-execs (Linux) and the restarted supervisor also
#                  has a working console.
#
# Usage: test/syscontrol.sh [wanted-cli] [config]
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=${2:-./docs/example_config_wsh.json}

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wsh engine not found at $WANTED (run 'make wsh')"
    exit 1
fi

HELP_MARKER="Following commands are available"
rc=0

# Boot the engine with a console FIFO, run the timed "delay:command" steps, and
# leave the captured console in $OUT and engine liveness in $ALIVE (1/0). The
# engine is killed by PID (never via a timeout wrapper, which would orphan it).
ENGINE_OUT=""
ENGINE_ALIVE=0
drive() {
    local fifo ep
    ENGINE_OUT=$(mktemp)
    fifo=$(mktemp -u)
    mkfifo "$fifo"

    "$WANTED" "$CONFIG" <"$fifo" >"$ENGINE_OUT" 2>&1 &
    ep=$!
    exec 9>"$fifo"          # hold the write end open: no EOF until we close it

    local spec
    for spec in "$@"; do
        sleep "${spec%%:*}"
        printf '%s\n' "${spec#*:}" >&9
    done
    sleep 2

    if kill -0 "$ep" 2>/dev/null; then
        ENGINE_ALIVE=1
    else
        ENGINE_ALIVE=0
    fi
    exec 9>&-
    kill -9 "$ep" 2>/dev/null
    wait "$ep" 2>/dev/null
    rm -f "$fifo"
}

check() {
    if [ "$1" -eq 0 ]; then
        echo "ok   - $2"
    else
        echo "FAIL - $2"
        rc=1
    fi
}

# 1. poweroff -> engine exits, no respawn.
drive "1:poweroff"
[ "$ENGINE_ALIVE" -eq 0 ]; check $? "poweroff exits the engine without respawn"
rm -f "$ENGINE_OUT"

# 2. exit -> respawn, and the respawned supervisor has a working console.
#    The first wsh only receives 'exit'; the help listing can therefore only come
#    from the respawned wsh processing the later 'help'.
drive "1:exit" "3:help"
[ "$ENGINE_ALIVE" -eq 1 ]; check $? "exit respawns the supervisor (engine stays up)"
grep -q "$HELP_MARKER" "$ENGINE_OUT"
check $? "respawned supervisor has a working console (post-exit command runs)"
rm -f "$ENGINE_OUT"

# 3. reboot -> re-exec, and the restarted supervisor has a working console.
drive "1:reboot" "3:help"
grep -q "$HELP_MARKER" "$ENGINE_OUT"
check $? "rebooted supervisor has a working console (post-reboot command runs)"
rm -f "$ENGINE_OUT"

if [ "$rc" -eq 0 ]; then
    echo "PASS: syscontrol (poweroff / exit-respawn / reboot)"
else
    echo "FAIL: syscontrol"
fi
exit $rc
