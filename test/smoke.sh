#!/bin/sh
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
    pattern="$2"
    input="$3"
    result=$(printf '%s\n' "$input" | "$WANTED" "$CONFIG" 2>&1)
    if printf '%s\n' "$result" | grep -q "$pattern"; then
        printf 'PASS [%s]\n' "$label"
        PASS=$((PASS + 1))
    else
        printf 'FAIL [%s]: expected pattern "%s" in output\n' "$label" "$pattern"
        printf '%s\n' "$result"
        FAIL=$((FAIL + 1))
    fi
}

# Root directory listing must expose VFS namespaces
check "root lists dev"          "dev"           "ls /"
check "root lists net"          "net"           "ls /"
check "root lists proc"         "proc"          "ls /"

# /dev namespace
check "dev lists null"          "null"          "ls /dev"
check "dev lists pipe"          "pipe"          "ls /dev"
check "dev lists stdin"         "stdin"         "ls /dev"
check "dev trailing slash"      "null"          "ls /dev/"

# /net namespace
check "net readdir"             "\."            "ls /net"

# Path normalization: cd .. must resolve correctly
check "dotdot from dev"         "dev"           "cd /dev && ls .."
check "dotdot from wanted"      "wanted"        "cd /dev/wanted && ls .. | grep wanted"

# /dev/null: read returns empty
check "null read"               ""              "cat /dev/null"

# /proc entries (require privileged: true in config)
check "proc wapps"              "name:"         "cat /proc/wapps"
check "proc memory"             "stack_size:"   "cat /proc/memory"

# /dev/pipe roundtrip
check "pipe roundtrip"          "hello"         "echo hello > /dev/pipe/t & cat /dev/pipe/t"

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
