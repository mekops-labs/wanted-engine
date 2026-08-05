#!/bin/bash
# Shared helper for test harnesses that stage wapps/<name>/<name>.wasm. The wasm
# toolchain lives in the wapp-sdk image, so WAPPS_PREBUILT=1 requires the .wasm
# to exist already; unset, it compiles on demand.
#
# In:  WAPP_ROOT — repo root the wapps/ tree hangs off (default: CWD)

wapp_build() {
    local name=$1 root=${WAPP_ROOT:-.} wasm
    wasm="$root/wapps/$name/$name.wasm"

    if [ "${WAPPS_PREBUILT:-0}" = 1 ]; then
        [ -f "$wasm" ] || {
            echo "FAIL: WAPPS_PREBUILT=1 but $wasm is missing"
            exit 1
        }
        return 0
    fi

    make -C "$root/wapps/$name" >/dev/null 2>&1 || {
        echo "FAIL: build wapps/$name"
        exit 1
    }
}
