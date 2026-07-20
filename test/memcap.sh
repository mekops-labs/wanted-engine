#!/bin/bash
# Negative test for WASM_MAX_MEMORY_PAGES — both halves of the enforcement.
#
# bigmem  starts at one page and grows ~160 KiB, exercising the runtime growth
#         cap: under a 1-page cap memory.grow is refused, malloc returns NULL and
#         it logs "bigmem-bounded"; under a wider cap it logs "bigmem-reached".
# biginit declares four initial pages, exercising the load-time check: under a
#         cap below four pages the engine refuses to load it (no marker); under a
#         cap of four it loads and logs "biginit-loaded".
#
# Builds the wsh engine at cap=1 and cap=4 and drives each wapp over the console.
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
WAPPS="bigmem biginit"

[ -f "$WSH_TAR" ] || { echo "FAIL: missing $WSH_TAR (run 'make supervisor')"; exit 1; }

mkdir -p "$REG"
for w in $WAPPS; do
    wapp_build "$w"
    s=$(mktemp -d)
    cp "wapps/$w/$w.wasm" "$s/app.wasm"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$REG/$w:0.0.1-1.wapp" app.wasm
    rm -rf "$s"
done
trap 'for w in $WAPPS; do rm -f "$REG/$w":*.wapp; done; rm -rf build-memcap-*' EXIT

build_cli() { # $1 = max pages -> path to the built CLI
    local d="build-memcap-$1"
    rm -rf "$d" && mkdir "$d"
    ( cd "$d" && cmake -G Ninja -DWANTED_SUPERVISOR_IMAGE_PATH="$WSH_TAR" \
        -DWASM_MAX_MEMORY_PAGES="$1" "$ROOT" >/dev/null 2>&1 \
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
    printf 'cat /dev/wanted/wapps/%s/log\n' "$name" >&9; sleep 1
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

[ "$rc" -eq 0 ] && echo "PASS: memcap (WASM_MAX_MEMORY_PAGES enforced on growth and initial memory)"
exit "$rc"
