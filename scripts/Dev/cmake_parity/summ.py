import json,collections,sys
r=json.load(open('F:/aftmp/cmake3/report.json'))
agg=collections.Counter(); ex={}
for n,cs in r.items():
    for c,v in cs.items():
        if 'presence' in v: print('PRES',n,c,v['presence']); continue
        s=v['sources']
        if s['premake_only'] or s['cmake_only']: print('SRC',n,c,len(s['premake_only']),len(s['cmake_only']), (s['premake_only']+s['cmake_only'])[:2])
        for d in v['compile']:
            for k,x in d['diff'].items():
                if k in ('I','EI'): agg[(k,n)]+=1
                else:
                    for t in x: agg[(k,t)]+=1; ex.setdefault((k,t),set()).add(n+':'+c)
for k,v in sorted(agg.items(), key=lambda x:-x[1]): print(v,k, sorted(ex.get(k,[]))[:6] if len(ex.get(k,[]))<200 else '')
