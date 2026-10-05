"""queen_lastmove.py FOLDER : for each head-on death of our queen, her options at the start of the fatal
round and the one before: each neighbour cell - legal?, inside an enemy's reach this turn (free+paid,
manhattan), free exits after the move. 'had a safe move' = some legal option outside all reaches."""
import sys, os, re, glob, hashlib, collections
from ladderlog import Game
def reach(L): return (L+3)//4 + max(0, L-2)
root=sys.argv[1]; seen=set(); summary=collections.Counter()
for p in sorted(glob.glob(os.path.join(root,'*','Elo-*.txt'))):
    if sum(1 for _ in open(p))<5: continue
    h=hashlib.md5(open(p,'rb').read()).hexdigest()
    if h in seen: continue
    seen.add(h); g0=Game(p); g0.run()
    if g0.deathwhy.get(g0.US)!='head-on collision': continue
    g=Game(p); m=g.map; Q=g.US; g.died=dict(g0.died)
    D=g0.died[Q]; out={}
    def onr(gg,r):
        if r-1 not in (D-1, D-2) or Q not in gg.alive: return
        occ=gg.occupied(); b=gg.body[Q]; hd=b[0]
        enem=[(gg.body[j][0],len(gg.body[j])) for j in gg.alive if gg.tm(j)!=gg.US and gg.body[j]]
        opts=[]
        for d in 'NESW':
            if m.kelp_edge(hd[0],hd[1],d): continue
            n=m.nb(hd[0],hd[1],d)
            if n in occ and not (occ[n][0]==Q and occ[n][1]==len(b)-1): continue
            hit=min((m.dist(e,n)-reach(l) for e,l in enem), default=99)
            ex=sum(1 for d2 in 'NESW' if not m.kelp_edge(n[0],n[1],d2) and m.nb(n[0],n[1],d2)!=hd and m.nb(n[0],n[1],d2) not in occ)
            opts.append((d, 'IN-REACH' if hit<=0 else f'safe(+{hit})', ex))
        out[r]=(len(b),opts)
    g.run(onround=onr)
    name=os.path.relpath(p,root)[:-4]
    fatal=out.get(D); before=out.get(D-1)
    s_f=any(o[1].startswith('safe') for o in fatal[1]) if fatal else None
    s_b=any(o[1].startswith('safe') for o in before[1]) if before else None
    summary[('fatal round: safe move existed' if s_f else 'fatal round: no safe move', 'round before: safe existed' if s_b else 'round before: none')]+=1
    print(f'{name:<28} L{fatal[0] if fatal else "?"}  fatal-round options {fatal[1] if fatal else ""}\n{"":28}      round-before options {before[1] if before else ""}')
print()
for k,v in summary.most_common(): print(v,k)
