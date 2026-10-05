"""doubledeath.py FOLDER : survival splits (save/trap/unwind/queen-stall) where parent AND child both die
within 2 rounds; whether the split happened inside a portal box or a fountain pocket; per game and map.
Also counts all our dragon deaths whose head was in a cell reached through a portal in the last 3 moves."""
import sys, os, re, glob, hashlib, collections
from ladderlog import Game, DIRS
def fast_tiles(g):
    from ladderlog import MAPDIR
    s=set()
    for l in open(os.path.join(MAPDIR, g.mapkey+'.map')):
        p=l.split()
        if p and p[0]=='TILE' and p[1]!='COUNT' and p[3]!='0' and int(p[4])<=5: s.add((int(p[1]),int(p[2])))
    return s
root=sys.argv[1]; seen=set(); tot=collections.Counter(); per=collections.defaultdict(collections.Counter)
for p in sorted(glob.glob(os.path.join(root,'*','Elo-*.txt'))):
    if sum(1 for _ in open(p))<5: continue
    h=hashlib.md5(open(p,'rb').read()).hexdigest()
    if h in seen: continue
    seen.add(h)
    g=Game(p); m=g.map; fast=fast_tiles(g)
    splits={}; portal_at={}; prev={}; st={}
    def oe(gg,r,kind,i,d):
        if kind=='log': st[i]=d
        elif kind=='move':
            pv=prev.get(i)
            if pv is not None and m.portal_edge(pv[0],pv[1],d['dir']):
                dx,dy=DIRS[d['dir']]
                if ((pv[0]+dx)%m.W,(pv[1]+dy)%m.H)!=d['to']: portal_at[i]=r
            prev[i]=d['to']
        elif kind=='split' and gg.side(i)=='us':
            w=re.findall(r'(save-split|trap-split|unwind-split|queen-stall|insure-split|grow-fleet|lane-split|pearl-split|half-split|post-trim|bunker-trim|queen-split|keep-fleet)', st.get(i,''))
            head=gg.body[i][0] if gg.body.get(i) else None
            splits[d['child']]=(r,i,w[0] if w else '?', r-portal_at.get(i,-99)<=6, head in fast or any(c in fast for c in gg.body.get(i,[])[:4]))
            prev[d['child']]=None
    g.run(onevent=oe)
    name=os.path.relpath(p,root)[:-4]
    for c,(r,par,w,viaP,fnt) in splits.items():
        dc=g.died.get(c); dp=g.died.get(par)
        both = dc is not None and dp is not None and dc-r<=2 and dp-r<=2
        key=(w,'both died' if both else 'child died' if dc is not None and dc-r<=2 else 'ok', 'via portal' if viaP else '', 'fountain' if fnt else '')
        per[g.mapkey][key]+=1; tot[key]+=1
for mp,c in per.items():
    print(f'== {mp}')
    for k,v in c.most_common():
        if k[1]!='ok' or k[0] in ('save-split','trap-split'): print(f'   {v:4} {k}')
