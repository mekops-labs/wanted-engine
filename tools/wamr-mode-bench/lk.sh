#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# lk.sh <cli> <screen> <mode> <hz> <secs> <enc> <out>
cli=$1; screen=$2; mode=$3; hz=$4; secs=$5; enc=$6; out=$7
B=$(cd "$(dirname "$0")" && pwd); cd "$B/lk" || exit
cs=$((secs+6)); [ "$mode" = idle ] && cs=0
python3 -I mkconfig.py "$screen" "$mode" "$hz" $cs > cfg.json
fmt=rgb565; [ "$screen" = tender ] && fmt=gray4
( trap "" PIPE; exec "$cli" cfg.json ) > "$out.log" 2>&1 &
pid=$!
for i in $(seq 1 100); do grep -q "flushing" "$out.log" && break; sleep 0.1; done
cpu() { awk '{print $14+$15}' /proc/$pid/stat; }
tick=$(getconf CLK_TCK)
c0=$(cpu); t0=$(date +%s.%N)
python3 -I "$B/rfbclient.py" 5900 "$secs" $fmt "$enc" > "$out.json" 2>"$out.err"
c1=$(cpu); t1=$(date +%s.%N)
# per-thread CPU split
for t in /proc/$pid/task/*; do echo "$(basename "$t") $(awk '{print $14+$15}' "$t"/stat)"; done > "$out.threads"
kill -INT $pid; sleep 1; kill -KILL $pid 2>/dev/null
python3 -I - "$c0" "$c1" "$t0" "$t1" "$tick" "$out" <<'PY'
import sys,json
c0,c1,t0,t1,tick=map(float,sys.argv[1:6]); out=sys.argv[6]
try: d=json.load(open(out+'.json'))
except Exception: d={}
d['cpu_pct']=round((c1-c0)/tick/(t1-t0)*100,1)
json.dump(d,open(out+'.json','w')); print(json.dumps(d))
PY
