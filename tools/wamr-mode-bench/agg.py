import re,sys,glob,statistics as st,json
def load(mode):
    runs=[]
    for f in sorted(glob.glob(f'r-{mode}-*.txt')):
        d={}
        for l in open(f):
            m=re.match(r'wbench: (\S+) median_us=([\d.]+) min_us=([\d.]+) max_us=([\d.]+)',l)
            if m: d[m[1]]=(float(m[2]),float(m[3]),float(m[4]))
            m=re.match(r'wbench: start_realtime_ns=(\d+)',l)
            if m: d['_start']=int(m[1])
            m=re.match(r'(launch_ns|rss_at_start_kb|rss_end_kb|hwm_kb)=(\d+)',l)
            if m: d[m[1]]=int(m[2])
        if 'launch_ns' in d and '_start' in d: d['startup_ms']=(d['_start']-d['launch_ns'])/1e6
        runs.append(d)
    return runs
res={m:load(m) for m in('classic','fast')}
names=[k for k in res['classic'][0] if not k.startswith('_') and k not in('launch_ns','rss_at_start_kb','rss_end_kb','hwm_kb','startup_ms')]
out={}
for m,runs in res.items():
    out[m]={n:(st.median(r[n][0] for r in runs),min(r[n][1] for r in runs),max(r[n][2] for r in runs)) for n in names}
    for k in('startup_ms','rss_at_start_kb','rss_end_kb','hwm_kb'): out[m][k]=st.median(r[k] for r in runs)
print(len(res['classic']),len(res['fast']),'runs')
print(f"{'workload':<22}{'classic':>10}{'fast':>10}{'speedup':>9}")
for n in names:
    c,f=out['classic'][n][0],out['fast'][n][0]
    print(f"{n:<22}{c:>10}{f:>10}{(c/f if f else 0):>9.2f}")
for k in('startup_ms','rss_at_start_kb','rss_end_kb','hwm_kb'): print(k,out['classic'][k],out['fast'][k])
json.dump(out,open('linux.json','w'))
