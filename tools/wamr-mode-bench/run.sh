#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# run.sh <cli> <out>: run wbench once, record launch time and RSS
cli=$1; out=$2; B=$(dirname "$0")
: > "$out"
t0=$(date +%s%N)
"$cli" "$B/config.json" > "$out" 2>&1 &
pid=$!
rss0=""
for i in $(seq 1 2400); do
  if [ -z "$rss0" ] && grep -q "start_realtime" "$out"; then rss0=$(awk '/VmRSS/{print $2}' /proc/$pid/status); fi
  grep -q "wbench: done" "$out" && break
  sleep 0.1
done
hwm=$(awk '/VmHWM/{print $2}' /proc/$pid/status); rss1=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
kill -INT $pid; sleep 1; kill -KILL $pid 2>/dev/null
echo "launch_ns=$t0" >> "$out"; echo "rss_at_start_kb=$rss0" >> "$out"; echo "rss_end_kb=$rss1" >> "$out"; echo "hwm_kb=$hwm" >> "$out"
