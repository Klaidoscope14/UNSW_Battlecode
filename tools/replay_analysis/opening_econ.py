"""opening_econ.py FOLDER : first 150 rounds per game - pearls eaten by r50/r100/r150 per side, how many
of ours the queen ate, our queen's status mix in r1-100, splits per side by r100, dragons at r100/r150."""
import sys, os, re, glob, hashlib, collections, statistics as st
from ladderlog import Game
def word(t):
    for w in re.findall(r'[a-z][a-z-]+', t or ''):
        if w not in ('cmb','col','exp','prt','nrm','war','keep','farm'): return w
    return ''
root=sys.argv[1]; seen=set(); rows=[]
for p in sorted(glob.glob(os.path.join(root,'*','Elo-*.txt'))):
    if sum(1 for _ in open(p))<5: continue
    h=hashlib.md5(open(p,'rb').read()).hexdigest()
    if h in seen: continue
    seen.add(h); g=Game(p)
    eat=collections.Counter(); qeat=collections.Counter(); spl=collections.Counter(); qst=collections.Counter(); n={}
    def oe(gg,r,kind,i,d):
        if r>150: return
        s=gg.side(i) if i is not None else None
        if kind=='move' and d['eat']:
            for c in (50,100,150):
                if r<=c: eat[(s,c)]+=1; qeat[(s,c)]+= i in (0,1)
        elif kind=='split':
            if r<=100: spl[s]+=1
        elif kind=='log' and i==gg.US and r<=100: qst[word(d)]+=1
    def orr(gg,r):
        if r-1 in (100,150): n[r-1]=(sum(1 for j in gg.alive if gg.tm(j)==gg.US),sum(1 for j in gg.alive if gg.tm(j)==gg.OPP))
    g.run(onround=orr,onevent=oe)
    rows.append((g.mapkey,os.path.basename(p)[:-4],eat,qeat,spl,qst,n))
by=collections.defaultdict(list)
for r in rows: by[r[0]].append(r)
for m,R in sorted(by.items()):
    med=lambda f:st.median([f(r) for r in R])
    print(f"{m:14} pearls r50 {med(lambda r:r[2][('us',50)]):>4}-{med(lambda r:r[2][('opp',50)]):<4} r100 {med(lambda r:r[2][('us',100)]):>4}-{med(lambda r:r[2][('opp',100)]):<4} r150 {med(lambda r:r[2][('us',150)]):>4}-{med(lambda r:r[2][('opp',150)]):<5}"
          f" queen ate (r100) {med(lambda r:r[3][('us',100)]):>3}-{med(lambda r:r[3][('opp',100)]):<3} splits r100 {med(lambda r:r[4]['us']):>3}-{med(lambda r:r[4]['opp']):<3}"
          f" dragons r100 {med(lambda r:r[6].get(100,(0,0))[0]):>3}-{med(lambda r:r[6].get(100,(0,0))[1]):<3} r150 {med(lambda r:r[6].get(150,(0,0))[0]):>3}-{med(lambda r:r[6].get(150,(0,0))[1]):<3}")
    q=collections.Counter()
    for r in R: q.update(r[5])
    tot=sum(q.values()) or 1
    print(f"{'':14} our queen r1-100: "+', '.join(f'{k} {100*v/tot:.0f}%' for k,v in q.most_common(6)))
