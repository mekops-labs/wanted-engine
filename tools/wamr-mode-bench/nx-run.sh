#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# nx-run.sh <nuttx-binary> <out>
bin=$1; out=$2; B=$(cd "$(dirname "$0")" && pwd)
cd "$B"/nuttx/simroot || exit; : > "$out"
t0=$(date +%s%N)
"$bin" < /dev/null > "$out" 2>&1 &
pid=$!
rss0=""; first=""
for i in $(seq 1 3000); do
  if [ -z "$first" ] && grep -q "start_realtime" "$out"; then first=$(( ($(date +%s%N)-t0)/1000000 )); rss0=$(awk '/VmRSS/{print $2}' /proc/$pid/status); fi
  grep -q "wbench: done" "$out" && break
  sleep 0.1
done
hwm=$(awk '/VmHWM/{print $2}' /proc/$pid/status); rss1=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
kill -INT $pid 2>/dev/null; sleep 1; kill -KILL $pid 2>/dev/null
echo "startup_first_line_ms=$first" >> "$out"; echo "rss_at_start_kb=$rss0" >> "$out"; echo "rss_end_kb=$rss1" >> "$out"; echo "hwm_kb=$hwm" >> "$out"
