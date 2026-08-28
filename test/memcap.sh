#!/bin/bash
# Negative test for WASM_MAX_MEMORY_PAGES, covering both halves of the
# enforcement. It builds the wsh engine at cap=1 and cap=4 and drives bigmem
# and biginit over the console; see the platform guide.
#
# Usage: test/memcap.sh   (runs in the build container; see `make memcap`)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
# shellcheck source=test/lib-wapp.sh
. "$ROOT/test/lib-wapp.sh"
WSH_TAR=./wasm/supervisor/wsh/supervisor.tar
CONFIG=./configs/example_config_wsh.json
REG=${REGISTRY_ROOT:-./registry}
WAPPS="bigmem biginit looper"

[ -f "$WSH_TAR" ] || { echo "FAIL: missing $WSH_TAR (run 'make supervisor')"; exit 1; }

mkdir -p "$REG"
for w in $WAPPS; do
    wapp_build "$w"
    s=$(mktemp -d)
    cp "wapps/$w/$w.wasm" "$s/app.wasm"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$REG/$w@0.0.1-1.wapp" app.wasm
    rm -rf "$s"
done
trap 'for w in $WAPPS; do rm -f "$REG/$w"@*.wapp; done; rm -rf build-memcap-*' EXIT

build_cli() { # $1 = max pages -> path to the built CLI
    local d="build-memcap-$1" kcl="$ROOT/tools/kconfiglib"
    rm -rf "$d" && mkdir "$d"
    # The page cap is a Kconfig symbol, not a cmake -D: seed the build dir with
    # the defaults, then set the one value this run varies.
    ( export PYTHONPATH="$kcl" KCONFIG_CONFIG="$d/.config"
      python3 "$kcl/olddefconfig.py" Kconfig >/dev/null 2>&1 \
      && python3 "$kcl/setconfig.py" --kconfig Kconfig \
           "WANTED_WASM_MAX_MEMORY_PAGES=$1" >/dev/null 2>&1 ) \
        || { echo "kconfig fail" >&2; return 1; }
    ( cd "$d" && cmake -G Ninja "$ROOT" >/dev/null 2>&1 \
        && ninja cmd/wanted-cli >/dev/null 2>&1 ) || { echo "build fail" >&2; return 1; }
    echo "$d/cmd/wanted-cli"
}

# Drive wsh: create/config/start the named wapp, then read its log. Prints the
# captured console (each wapp writes a distinct marker, or none if refused).
run_wapp() { # $1 = cli path, $2 = wapp name
    local cli="$1" name="$2" fifo out ep
    out=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    "$cli" "$CONFIG" <"$fifo" >"$out" 2>&1 &
    ep=$!
    exec 9>"$fifo"
    sleep 1
    printf 'create %s\n' "$name" >&9; sleep 1
    printf 'set_config %s {"image":"%s"}\n' "$name" "$name" >&9; sleep 1
    printf 'start %s\n' "$name" >&9; sleep 1
    printf 'cat /logs/%s\n' "$name" >&9; sleep 1
    exec 9>&-
    kill -9 "$ep" 2>/dev/null; wait "$ep" 2>/dev/null
    rm -f "$fifo"
    cat "$out"
    rm -f "$out"
}

# Drive wsh: start a long-running wapp, then read /proc/memory with it live.
run_procmem() { # $1 = cli path, $2 = wapp name
    local cli="$1" name="$2" fifo out ep
    out=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    "$cli" "$CONFIG" <"$fifo" >"$out" 2>&1 &
    ep=$!
    exec 9>"$fifo"
    sleep 1
    printf 'create %s\n' "$name" >&9; sleep 1
    printf 'set_config %s {"image":"%s"}\n' "$name" "$name" >&9; sleep 1
    printf 'start %s\n' "$name" >&9; sleep 2
    printf 'cat /proc/wapps/%s/memory\n' "$name" >&9; sleep 1
    printf 'cat /proc/memory\n' >&9; sleep 1
    exec 9>&-
    kill -9 "$ep" 2>/dev/null; wait "$ep" 2>/dev/null
    rm -f "$fifo"
    cat "$out"
    rm -f "$out"
}

# check <label> <output> <expected-substring|!absent> <pass-msg> <fail-msg>
rc=0
check() {
    echo "$2" | sed 's/^/  /'
    if echo "$2" | grep -q "$3"; then
        echo "PASS: $4"
    else
        echo "FAIL: $5"; rc=1
    fi
}
check_absent() {
    echo "$2" | sed 's/^/  /'
    if echo "$2" | grep -q "$3"; then
        echo "FAIL: $5"; rc=1
    else
        echo "PASS: $4"
    fi
}

cli1=$(build_cli 1)
cli4=$(build_cli 4)

echo "=== runtime growth cap ==="
echo "-- cap=1: bigmem grow must be BOUNDED --"
check "" "$(run_wapp "$cli1" bigmem)" 'bigmem-bounded' \
    "grow refused under a 1-page cap" "bigmem was not bounded under a 1-page cap"
echo "-- cap=4: bigmem grow must be ADMITTED --"
check "" "$(run_wapp "$cli4" bigmem)" 'bigmem-reached' \
    "bigmem grew to the target under a 4-page cap" "bigmem did not reach the target under a 4-page cap"

echo "=== load-time initial-memory check ==="
echo "-- cap=1: biginit (4 initial pages) must be REFUSED at load --"
check_absent "" "$(run_wapp "$cli1" biginit)" 'biginit-loaded' \
    "biginit refused under a 1-page cap" "biginit loaded despite exceeding a 1-page cap"
echo "-- cap=4: biginit must LOAD --"
check "" "$(run_wapp "$cli4" biginit)" 'biginit-loaded' \
    "biginit loaded under a 4-page cap" "biginit did not load under a 4-page cap"

echo "=== /proc/memory free-page accounting ==="
# looper declares max == initial, so its own headroom is zero. With slots still
# free the engine can nonetheless commit more pages, and the figure must say so
# — reporting only loaded-wapp headroom made this a constant zero.
mem=$(run_procmem "$cli4" looper)
echo "$mem" | sed 's/^/  /'
free=$(echo "$mem" | sed -n 's/^wasm_pages_free:[[:space:]]*//p' | tr -d '\r')
if echo "$mem" | grep -q 'pages_cur:[[:space:]]*1' && \
   echo "$mem" | grep -q 'pages_max:[[:space:]]*1'; then
    echo "PASS: a max==initial wapp reports no headroom of its own"
else
    echo "FAIL: looper did not report pages_cur == pages_max == 1"; rc=1
fi
if [ -n "$free" ] && [ "$free" -gt 0 ] 2>/dev/null; then
    echo "PASS: wasm_pages_free counts free slots ($free pages)"
else
    echo "FAIL: wasm_pages_free is '${free:-unset}' with wapp slots still free"; rc=1
fi

[ "$rc" -eq 0 ] && echo "PASS: memcap (WASM_MAX_MEMORY_PAGES enforced on growth and initial memory)"
exit "$rc"
