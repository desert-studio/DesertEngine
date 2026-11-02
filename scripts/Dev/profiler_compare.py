#!/usr/bin/env python3
"""Per-pass GPU (or CPU) compare of two editor.log files, profile dump by profile dump.

Usage: profiler_compare.py <A/editor.log> <B/editor.log> [gpu|cpu] [rows]
The k-th "[Profiler] ---- per-pass breakdown" dump of A is compared with the k-th of B (FPS-FIELD-BARN:
1_static, 1_static_vis, 1_foliage_low, 1_foliage_epic, 2_wind). Counts in parentheses ("(64 pages)") are
folded, so a row whose name carries a per-frame count sums into one. Prints the frame wall/GPU of both, the
RDG Wait / RDG Async / GPU Unaccounted totals, then the rows with the largest |A - B|."""
import sys,re
names=['1_static','1_static_vis','1_foliage_low','1_foliage_epic','2_wind']
def parse(p):
    dumps=[];cur=None
    for ln in open(p,encoding='utf-8',errors='replace'):
        i=ln.find('[Profiler] ')
        if i<0:
            continue
        s=ln[i+11:].rstrip('\n')
        if 'per-pass breakdown' in s:
            cur={'_frame':None,'cpu':{}};dumps.append(cur);continue
        if cur is None: continue
        m=re.match(r'Frame \(wall\) ([\d.]+) ms.*GPU frame ([\d.]+) ms',s)
        if m: cur['_frame']=(float(m.group(1)),float(m.group(2)));continue
        t=s.split()
        if len(t)<8: continue
        try: v=[float(x) for x in t[-7:]]
        except: continue
        name=re.sub(r'\(\d+[^)]*\)','(N)',' '.join(t[:-7]))
        cur[name]=cur.get(name,0.0)+v[2]; cur['cpu'][name]=cur['cpu'].get(name,0.0)+v[0]
    return dumps
a=parse(sys.argv[1]); b=parse(sys.argv[2])  
mode=sys.argv[3] if len(sys.argv)>3 else 'gpu'
for k,n in enumerate(names):
    if k>=len(a) or k>=len(b): break
    A=a[k];B=b[k]
    print('==',n,'wall/gpu A',A['_frame'],'B',B['_frame'])
    src_a=A if mode=='gpu' else A['cpu']; src_b=B if mode=='gpu' else B['cpu']
    rows=[]
    for s in set(src_a)|set(src_b):
        if s.startswith('_') or s=='cpu': continue
        va=src_a.get(s,0.0); vb=src_b.get(s,0.0)
        if mode=='gpu' and va==0 and vb==0: continue
        rows.append((va-vb,s,va,vb))
    rows.sort(key=lambda r:-abs(r[0]))
    if mode=='gpu':
        for cat in ['RDG Wait','RDG Async','GPU Unaccounted']:
            sa=sum(r[2] for r in rows if cat in r[1]); sb=sum(r[3] for r in rows if cat in r[1])
            print('  [%s] a %.3f b %.3f d %+.3f'%(cat,sa,sb,sa-sb))
        sa=sum(r[2] for r in rows if 'scene view' in r[1] and 'RDG Wait' not in r[1] and 'RDG Async' not in r[1]); sb=sum(r[3] for r in rows if 'scene view' in r[1] and 'RDG Wait' not in r[1] and 'RDG Async' not in r[1])
        print('  [graphics passes] a %.3f b %.3f d %+.3f'%(sa,sb,sa-sb))
    for d,s,va,vb in rows[:int(sys.argv[4]) if len(sys.argv)>4 else 10]:
        print('  %+.3f  a %.3f b %.3f  %s'%(d,va,vb,s[:120]))
