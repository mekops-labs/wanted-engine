import json,glob,statistics as st
sc=[('term','full'),('term','row'),('tender','full'),('tender','tile'),('term','idle')]
out={}
for scr,mode in sc:
    for m in('classic','fast'):
        rs=[]
        for f in sorted(glob.glob(f'lkres/{m}-{scr}-{mode}-*.json')):
            try: rs.append(json.load(open(f)))
            except Exception: pass
        out[(scr,mode,m)]=rs
def med(rs,k):
    v=[r[k] for r in rs if r.get(k) is not None]
    return st.median(v) if v else None
print(f"{'scenario':<14}{'mode':<8}{'n':>2}{'updates':>9}{'p50ms':>8}{'p95ms':>8}{'cpu%':>7}")
rows={}
for scr,mode in sc:
    for m in('classic','fast'):
        rs=out[(scr,mode,m)]
        r=dict(n=len(rs),upd=med(rs,'updates'),p50=med(rs,'p50_ms'),p95=med(rs,'p95_ms'),cpu=med(rs,'cpu_pct'))
        rows[f'{scr}-{mode}-{m}']=r
        f=lambda x:'-' if x is None else f'{x:.1f}'
        print(f"{scr+' '+mode:<14}{m:<8}{r['n']:>2}{f(r['upd']):>9}{f(r['p50']):>8}{f(r['p95']):>8}{f(r['cpu']):>7}")
json.dump(rows,open('lk.json','w'))
