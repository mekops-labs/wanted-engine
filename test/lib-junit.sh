#!/bin/bash
# Shared helper: wraps a command from the outside, recording a JUnit testcase
# from its exit code and output, for harnesses that are plain shell scripts
# rather than CTest tests. Sourced, not executed.

_JUNIT_XML=""
_JUNIT_FAILURES=0
_JUNIT_COUNT=0

_junit_escape() {
    sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

# junit_run <name> -- <command...>
junit_run() {
    local name=$1 start end dur out rc esc_out
    shift
    [ "${1:-}" = "--" ] && shift

    start=$(date +%s.%N)
    out=$("$@" 2>&1)
    rc=$?
    end=$(date +%s.%N)
    dur=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

    printf '%s\n' "$out"
    esc_out=$(printf '%s' "$out" | _junit_escape)

    _JUNIT_COUNT=$((_JUNIT_COUNT + 1))
    if [ "$rc" -eq 0 ]; then
        _JUNIT_XML="${_JUNIT_XML}    <testcase name=\"${name}\" time=\"${dur}\"><system-out>${esc_out}</system-out></testcase>
"
    else
        _JUNIT_FAILURES=$((_JUNIT_FAILURES + 1))
        _JUNIT_XML="${_JUNIT_XML}    <testcase name=\"${name}\" time=\"${dur}\"><failure message=\"exit code ${rc}\">${esc_out}</failure></testcase>
"
    fi
    return "$rc"
}

# junit_write <file> <suite-name>
junit_write() {
    local file=$1 suite=${2:-integration}
    mkdir -p "$(dirname "$file")"
    {
        printf '<?xml version="1.0" encoding="UTF-8"?>\n'
        printf '<testsuites>\n'
        printf '  <testsuite name="%s" tests="%d" failures="%d">\n' "$suite" "$_JUNIT_COUNT" "$_JUNIT_FAILURES"
        printf '%s' "$_JUNIT_XML"
        printf '  </testsuite>\n'
        printf '</testsuites>\n'
    } > "$file"
}

junit_exit_code() {
    [ "$_JUNIT_FAILURES" -eq 0 ]
}
