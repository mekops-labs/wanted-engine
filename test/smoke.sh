#!/bin/bash
# Smoke tests for VFS namespace correctness via wsh supervisor.
# Each check pipes a single command to wanted-cli and asserts the output.
# Tests are acceptance criteria for the VFS/wsh gap fixes; some will fail
# until the corresponding gap is implemented.
set -e

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=${2:-./docs/example_config_smoke.json}

PASS=0
FAIL=0

check() {
    label="$1"
    [ "$2" == "!" ] && inverse=1 || inverse=0
    pattern="$3"
    input="$4"
    result=$(printf "${input}\n" | "$WANTED" "$CONFIG" 2>&1)
    if printf '%s\n' "$result" | grep -q  "$pattern" && ok=1 || ok=0;
        [ $ok -ne $inverse ]; then

        printf 'PASS [%s]\n' "$label"
        PASS=$((PASS + 1))
    else
        if [ "$inverse" -eq 0 ]; then
            printf 'FAIL [%s]: expected pattern "%s" in output\n' "$label" "$pattern"
        else
            printf 'FAIL [%s]: unexpected pattern "%s" in output\n' "$label" "$pattern"
        fi
        printf '%s\n' "$result"
        FAIL=$((FAIL + 1))
    fi
}

check "root lists app.wasm"      "" "app.wasm"           "ls /"
check "root lists manifest.json" "" "manifest.json"      "ls /"

# Root directory listing must expose VFS namespaces
check "root lists dev"         "" "dev"            "ls /"
check "root lists net"         "" "net"            "ls /"
check "root lists proc"        "" "proc"           "ls /"

# /dev namespace
check "dev lists null"         "" "null"           "ls /dev"
check "dev lists pipe"         "" "pipe"           "ls /dev"
check "dev lists stdin"        "" "stdin"          "ls /dev"
check "dev trailing slash"     "" "null"           "ls /dev/"

# /net namespace
check "net readdir"            "!" "No such file"  "ls /net"

# Path normalization: cd .. must resolve correctly
check "dotdot from dev"        "" "dev"            "cd /dev\nls .."
check "dotdot from wanted"     "" "wanted"         "cd /dev/wanted\nls .."

# /dev/null: read returns empty
check "null read"              "!" "No such file"  "cat /dev/null"

# /proc entries (require privileged: true in config)
check "proc wapps"             "" "name:"          "cat /proc/wapps"
check "proc memory"            "" "stack_size:"    "cat /proc/memory"

# /dev/pipe roundtrip
check "pipe roundtrip"         "" "hello"          "write /dev/pipe/t hello\ncat /dev/pipe/t"

# TarFS file read: content of manifest.json must be returned verbatim
check "tarfs cat manifest"     "" "supervisor"     "cat /manifest.json"

# /proc directory listing
check "proc lists wapps"       "" "wapps"          "ls /proc"
check "proc lists memory"      "" "memory"         "ls /proc"

# /dev/pipe readdir: pipe created by write must appear in directory listing
check "pipe appears in ls"     "" "smkpipe"        "write /dev/pipe/smkpipe x\nls /dev/pipe"

# TarFS is read-only: rm must be rejected
check "tarfs rm rejected"      "" "Read-only"      "rm /app.wasm"

# Negative: cat on a missing file must report an error
check "cat missing file"       "" "No such file"   "cat /no-such-file.txt"

# Negative: cd to a non-existent path must report an error
check "cd missing dir"         "" "No such file"   "cd /no-such-dir"

# WANTED control-plane namespace: path-addressed per-wapp control plane.
# The running supervisor is always present, so it is a stable probe target.
check "wanted lists ctl"       "" "ctl"          "ls /dev/wanted"
check "wanted lists wapps"     "" "wapps"        "ls /dev/wanted"
check "wapps lists supervisor" "" "supervisor"   "ls /dev/wanted/wapps"
check "wapp dir lists ctl"     "" "ctl"          "ls /dev/wanted/wapps/supervisor"
check "wapp dir lists state"   "" "state"        "ls /dev/wanted/wapps/supervisor"
check "supervisor state read"  "" "running"      "cat /dev/wanted/wapps/supervisor/state"
check "status enumerates"      "" "supervisor"   "status"
check "status names state"     "" "running"      "status supervisor"
check "absent wapp state"      "" "not_started"  "status no-such-wapp"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
